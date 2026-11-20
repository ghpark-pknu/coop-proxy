#!/usr/bin/env bash
# 8단계 검증: Valgrind memcheck — 메모리 누수와 잘못된 메모리 접근을 ASan과 다른 방법으로 한 번 더 확인한다
#
# ASan은 컴파일할 때 검사 코드를 끼워 넣는다. Valgrind는 이미 컴파일된 프로그램을 가상 CPU 위에서 명령 하나씩
# 실행하면서 모든 메모리 읽기·쓰기와 malloc/free를 추적한다.
# 방식이 달라서 한쪽이 놓친 문제를 다른 쪽이 잡을 수 있다. 둘은 함께 쓸 수 없으므로 새니타이저가 없는
# debug 빌드(-O0 -g)를 쓴다.
#
# 구성: 노드 2개 클러스터. node1만 Valgrind 위에서, node2는 보통으로 실행한다.
#   [1] 두 노드에 번갈아 요청 N개: node1이 직접 처리(HIT·MISS·교체·재검증), node2로 넘김(FORWARD),
#       node2가 넘겨 온 요청을 처리(peer). 캐시 1MB(교체가 계속), 검증 주기 1초(조건부 GET이 계속)
#   [2] 특수 요청: /__stats, /__dashboard, 차단 목록(403), 잘못된 요청(400), 없는 원서버(502), POST(501),
#       헤더를 보내다 멈춘 연결(송수신 제한 시간이 지나면 닫힘)
#   [3] node2를 끈 뒤 node1에 요청 N/3개: 담당 노드 연결 실패 → 2순위(자기 자신)로 대체하는 경로
#   끝: SIGTERM → 프록시가 정상 종료 절차(연결 정리, 캐시 해제)를 밟은 뒤 Valgrind가 보고서를 쓴다
#
# 판정: definitely lost 0, indirectly lost 0, 메모리 오류 0, 종료 때 프록시가 연 파일 0개(표준 입출력 제외)
#   definitely lost: 가리키는 포인터가 하나도 남지 않은 메모리 (확실한 누수)
#   indirectly lost: 누수된 블록 안에서만 가리키던 메모리 (누수의 딸림)
#   possibly lost  : 블록 중간을 가리키는 포인터만 남은 메모리 (참고만 한다)
#   still reachable: 종료 때까지 전역 변수 등이 가리키던 메모리 (누수 아님)
#
# 결과: results/memcheck_날짜_시각.log (Valgrind 보고서 전체)
set -u
cd "$(dirname "$0")/.." || exit 1

N=${N:-3000}
[[ "$N" =~ ^[1-9][0-9]*$ ]] || { echo "N은 1 이상의 정수여야 합니다 (입력값: $N)"; exit 1; }
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
P1=18811
P2=18812
ORIGIN_PORT=19810

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

if ! command -v valgrind > /dev/null 2>&1; then
    echo "valgrind가 없습니다: sudo apt install valgrind"; exit 1
fi
echo "[준비] debug 빌드 (새니타이저 없음) + release loadc"
make -s MODE=debug all && make -s MODE=release loadc || { echo "빌드 실패"; exit 1; }
BIN=build/debug/proxy
LC=build/release/loadc
if [ ! -f "$WWW/manifest.json" ]; then
    echo "[준비] 테스트 파일 생성 → $WWW"
    python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
fi
mkdir -p results "$LOGDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
VLOG="results/memcheck_$STAMP.log"
TMP=$(mktemp -d)
CONF="$TMP/cluster.conf"
printf 'node1 127.0.0.1 %d\nnode2 127.0.0.1 %d\n' $P1 $P2 > "$CONF"
V1=""
N2=""
ORIGIN_PID=""
cleanup() {
    [ -n "$V1" ] && kill -TERM "$V1" 2>/dev/null
    [ -n "$N2" ] && kill -TERM "$N2" 2>/dev/null
    [ -n "$ORIGIN_PID" ] && kill -TERM "$ORIGIN_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT
wait_port() {   # wait_port 포트 [제한 초]
    for _ in $(seq 1 $(( ${2:-10} * 10 ))); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "포트 $1 이 열리지 않았습니다 ($LOGDIR/memcheck_*.log, $VLOG 확인)"
    exit 1
}
for p in $ORIGIN_PORT $P1 $P2; do
    if nc -z 127.0.0.1 "$p" 2>/dev/null; then
        echo "포트 $p 를 이미 다른 프로그램이 쓰고 있습니다 (ss -ltnp | grep $p 로 확인 후 종료하세요)"
        exit 1
    fi
done
NOBJ=$(python3 -c "import json, sys; print(len(json.load(open(sys.argv[1]))['objects']))" "$WWW/manifest.json")

python3 tools/origin.py --port $ORIGIN_PORT --dir "$WWW" --quiet > "$LOGDIR/memcheck_origin.log" 2>&1 &
ORIGIN_PID=$!
wait_port $ORIGIN_PORT
# --track-fds=yes          : 종료 때 열려 있던 파일 디스크립터와 그것을 연 위치를 보고
# --errors-for-leak-kinds  : definite·indirect 누수를 "오류"로 센다 → ERROR SUMMARY와 종료 코드에 반영
# --error-exitcode=99      : 오류가 하나라도 있으면 종료 코드 99
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect,possible \
    --errors-for-leak-kinds=definite,indirect --track-fds=yes --num-callers=25 --error-exitcode=99 \
    --log-file="$VLOG" "$BIN" -n node1 -C "$CONF" -q -v 1 -s 1M -r 2000 -b conf/blocklist.txt \
    > "$LOGDIR/memcheck_node1.log" 2>&1 < /dev/null &
V1=$!
"$BIN" -n node2 -C "$CONF" -q -v 1 -s 1M -r 2000 > "$LOGDIR/memcheck_node2.log" 2>&1 < /dev/null &
N2=$!
wait_port $P2
wait_port $P1 60                       # Valgrind 위에서는 시작도 느리다
echo "[준비] node1(Valgrind, pid $V1), node2(pid $N2), 원서버, 객체 ${NOBJ}개 → 보고서 $VLOG"

echo "[1] 두 노드에 번갈아 요청 ${N}개 (동시 4)"
"$LC" -x 127.0.0.1:$P1,127.0.0.1:$P2 -o 127.0.0.1:$ORIGIN_PORT -k "$NOBJ" -z 0.8 -c 4 -n "$N" -t 30000 \
    > "$TMP/l1.txt" 2>&1
echo "    loadc 종료 코드 $? | $(grep -m1 '결과(마지막 X-Cache)' "$TMP/l1.txt" | sed 's/^ *//')"

echo "[2] 특수 요청"
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 30 "$@"; }
raw() {   # 날 요청을 그대로 보내고 상태 줄을 받는다
    python3 - "$P1" "$1" <<'EOF'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30)
s.sendall(sys.argv[2].encode().decode("unicode_escape").encode("latin-1"))
data = b""
while b"\r\n" not in data:
    chunk = s.recv(4096)
    if not chunk:
        break
    data += chunk
print(data.split(b"\r\n")[0].decode("latin-1", "replace") or "응답 없이 연결을 닫음")
EOF
}
echo "    /__stats             → $(code "http://127.0.0.1:$P1/__stats")"
echo "    /__dashboard         → $(code "http://127.0.0.1:$P1/__dashboard")"
echo "    차단 목록 blocked.test → $(code -x "http://127.0.0.1:$P1" "http://blocked.test/")"
echo "    없는 원서버 :9         → $(code -x "http://127.0.0.1:$P1" "http://127.0.0.1:9/x")"
echo "    POST                 → $(code -x "http://127.0.0.1:$P1" -X POST -d x "http://127.0.0.1:$ORIGIN_PORT/obj/0000.bin")"
echo "    잘못된 요청 줄          → $(raw 'GARBAGE\r\n\r\n')"
echo "    헤더 도중에 멈춘 요청    → $(raw 'GET http://127.0.0.1/ HTTP/1.0\r\nHost: x') (송수신 제한 5초)"

echo "[3] node2를 끄고 node1에만 요청 $((N / 3 + 1))개 (담당 노드 장애 → 2순위로 대체)"
kill -TERM "$N2"; wait "$N2"; N2=""
"$LC" -x 127.0.0.1:$P1 -o 127.0.0.1:$ORIGIN_PORT -k "$NOBJ" -z 0.8 -c 4 -n $((N / 3 + 1)) -t 30000 \
    > "$TMP/l3.txt" 2>&1
echo "    loadc 종료 코드 $? | $(grep -m1 '결과(마지막 X-Cache)' "$TMP/l3.txt" | sed 's/^ *//')"

echo "[끝] node1에 SIGTERM → 정상 종료 뒤 Valgrind 보고서 작성 (수십 초 걸릴 수 있음)"
kill -TERM "$V1"
wait "$V1"
vrc=$?
V1=""
kill -TERM "$ORIGIN_PID" 2>/dev/null; wait "$ORIGIN_PID" 2>/dev/null; ORIGIN_PID=""

python3 - "$VLOG" "$vrc" "$LOGDIR/memcheck_node1.log" <<'EOF'
import re, sys

vlog, vrc, nlog = sys.argv[1], int(sys.argv[2]), sys.argv[3]
text = open(vlog, encoding="utf-8", errors="replace").read()
# 각 줄 앞의 "==PID== "를 떼고 본다
lines = [re.sub(r"^==\d+== ?", "", l) for l in text.splitlines()]
body = "\n".join(lines)

def lost(kind):
    m = re.search(kind + r" lost: ([\d,]+) bytes in ([\d,]+) blocks", body)
    return (int(m.group(1).replace(",", "")), int(m.group(2).replace(",", ""))) if m else (0, 0)

no_leaks = "All heap blocks were freed -- no leaks are possible" in body
d, i, p = lost("definitely"), lost("indirectly"), lost("possibly")
m = re.search(r"still reachable: ([\d,]+) bytes in ([\d,]+) blocks", body)
reach = (int(m.group(1).replace(",", "")), int(m.group(2).replace(",", ""))) if m else (0, 0)
m = re.search(r"ERROR SUMMARY: ([\d,]+) errors from ([\d,]+) contexts", body)
errors = int(m.group(1).replace(",", "")) if m else -1
m = re.search(r"in use at exit: ([\d,]+) bytes in ([\d,]+) blocks", body)
inuse = m.group(1) if m else "?"
m = re.search(r"total heap usage: ([\d,]+) allocs, ([\d,]+) frees, ([\d,]+) bytes allocated", body)
heap = f"malloc {m.group(1)}번, free {m.group(2)}번, 모두 {m.group(3)}바이트" if m else "?"
# 종료 때 열린 파일: "Open ... N:" 블록 중 부모에게서 물려받은 것(<inherited from parent>)과 표준 입출력은 뺀다
own = []
for k, l in enumerate(lines):
    mm = re.match(r"Open (.*?) (\d+):", l) or re.match(r"Open (file descriptor) (\d+)", l)
    if mm:
        fd = int(mm.group(2))
        nxt = " ".join(lines[k + 1:k + 3])
        if fd > 2 and "inherited from parent" not in nxt:
            own.append(l.strip())
m = re.search(r"FILE DESCRIPTORS: (\d+) open \((\d+) std\) at exit", body)
fdline = f"{m.group(1)}개 (표준 입출력 {m.group(2)}개)" if m else "?"
ntext = open(nlog, encoding="utf-8", errors="replace").read()
clean = "종료: 연결" in ntext and "끝나지 않은 연결" not in ntext

print()
print(f"[Valgrind 보고서] {vlog}")
print(f"  힙 사용: {heap}, 종료 때 남은 메모리 {inuse}바이트")
if no_leaks:
    print("  누수: 없음 (All heap blocks were freed — 종료 때 해제되지 않은 블록이 하나도 없음)")
else:
    print(f"  definitely lost {d[0]:,}바이트({d[1]}블록) | indirectly lost {i[0]:,}바이트({i[1]}블록) | "
          f"possibly lost {p[0]:,}바이트({p[1]}블록) | still reachable {reach[0]:,}바이트({reach[1]}블록)")
print(f"  메모리 오류(잘못된 읽기·쓰기, 초기화 안 된 값 사용, 잘못된 free 등 + 위의 누수): {errors}개")
print(f"  종료 때 열린 파일: {fdline}, 그중 프록시가 열고 닫지 않은 것 {len(own)}개")
for o in own:
    print(f"    {o}")
print(f"  프록시 정상 종료: {'✓' if clean else '✗'} (Valgrind 종료 코드 {vrc}: 0이면 오류 없음, 99면 오류 있음)")
ok = d[0] == 0 and i[0] == 0 and errors == 0 and not own and clean and vrc == 0
print()
print("판정: " + ("통과 — 확실한 누수 0, 메모리 오류 0, 닫지 않은 파일 0" if ok
                 else f"확인 필요 — {vlog}에서 'definitely lost', 'Invalid', 'Open' 부분을 보세요"))
sys.exit(0 if ok else 1)
EOF

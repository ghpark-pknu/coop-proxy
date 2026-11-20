#!/usr/bin/env bash
# 6단계 측정 도구(C 부하 도구 loadc) 점검: 도구가 센 값이 프록시의 기록과 정확히 맞는가
#
# 시험용 포트: 프록시 18581·18582, 아무도 듣지 않는 포트 18589, 원서버 19500,
#             가짜 서버 19581(본문이 잘린 응답)·19582(404)
# 객체: ~/coop-data/www-small (1KB 객체 200개, 없으면 만든다)
set -u
cd "$(dirname "$0")/.." || exit 1

MODE=${MODE:-debug}
DATA=${COOP_DATA:-$HOME/coop-data}
SMALL="$DATA/www-small"
LOGDIR="$DATA/logs"
ORIGIN_PORT=19500
O="127.0.0.1:$ORIGIN_PORT"

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

pass=0
fail=0
ok() { printf '  \033[32m✓\033[0m %s\n' "$1"; pass=$((pass + 1)); }
ng() { printf '  \033[31m✗\033[0m %s\n' "$1"; fail=$((fail + 1)); }
expect() {      # 설명, 기대값, 실제값
    if [ "$3" = "$2" ]; then ok "$1 → $3"; else ng "$1 → ${3:-없음} (기대값 $2)"; fi
}
truth() {       # truth 설명 파이썬식 → 식이 참이면 통과
    if python3 -c "import sys; sys.exit(0 if ($2) else 1)" 2>/dev/null; then ok "$1"; else ng "$1"; fi
}
jget() {        # jget 파일 파이썬식 (d = JSON) → 값
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print($2)" "$1" 2>/dev/null
}
sget() {        # sget 포트 파이썬식 (d = /__stats JSON) → 값
    curl -s "http://127.0.0.1:$1/__stats" | python3 -c "import json,sys; d=json.load(sys.stdin); print($2)" 2>/dev/null
}

# ---------------------------------------------------------------- 준비
echo "[준비] 빌드 (MODE=$MODE)"
make -s MODE="$MODE" all loadc || { echo "빌드 실패"; exit 1; }
BIN="build/$MODE/proxy"
LC="build/$MODE/loadc"

if [ ! -f "$SMALL/manifest.json" ]; then
    echo "[준비] 1KB 객체 200개 생성 → $SMALL"
    python3 tools/gen_files.py --dir "$SMALL" --min 1024 --max 1024 > /dev/null || exit 1
fi
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/test6_*.log
TMP=$(mktemp -d)

declare -A PID=()
RCS=""
EXTRA_PIDS=""

wait_port() {
    for _ in $(seq 1 100); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "  포트 $1 이 열리지 않았습니다. $LOGDIR/test6_*.log 를 확인하세요."
    return 1
}
stop_proc() {
    kill -TERM "${PID[$1]}" 2>/dev/null
    wait "${PID[$1]}"
    RCS="$RCS$?"
    unset "PID[$1]"
}
cleanup() {
    for p in "${PID[@]}" $EXTRA_PIDS; do kill -TERM "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT
lc() {              # lc 로그이름 [loadc 옵션...] → 종료 코드. 출력은 $TMP/로그이름.txt, 경고는 test6_loadc.log에도
    local name=$1
    shift
    "$LC" -o "$O" -q "$@" > "$TMP/$name.txt" 2>&1
    local rc=$?
    cat "$TMP/$name.txt" >> "$LOGDIR/test6_loadc.log"
    return $rc
}

python3 tools/origin.py --port $ORIGIN_PORT --dir "$SMALL" --quiet > "$LOGDIR/test6_origin.log" 2>&1 &
EXTRA_PIDS="$EXTRA_PIDS $!"
# 가짜 서버: 19581은 Content-Length보다 짧은 본문, 19582는 404
python3 - > "$LOGDIR/test6_fake.log" 2>&1 <<'EOF' &
import socket, threading
def serve(port, reply):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port)); s.listen(64)
    while True:
        c, _ = s.accept()
        try:
            c.settimeout(2); data = b""
            while b"\r\n\r\n" not in data:
                chunk = c.recv(4096)
                if not chunk: break
                data += chunk
            if data: c.sendall(reply)
        except OSError:
            pass
        c.close()
threading.Thread(target=serve, args=(19581, b"HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\n0123456789"), daemon=True).start()
serve(19582, b"HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n")
EOF
EXTRA_PIDS="$EXTRA_PIDS $!"
"$BIN" -n A -p 18581 -q -v 600 >> "$LOGDIR/test6_A.log" 2>&1 &
PID[A]=$!
"$BIN" -n B -p 18582 -q -v 600 -S 8 >> "$LOGDIR/test6_B.log" 2>&1 &
PID[B]=$!
for p in $ORIGIN_PORT 19581 19582 18581 18582; do wait_port "$p" || exit 1; done

# ---------------------------------------------------------------- 점검
echo "[1] 요청 2000개, 동시 1, 빈 캐시 (프록시 A, -S 1): 도구의 집계 = 프록시의 기록"
lc n1 -x 127.0.0.1:18581 -n 2000 -c 1 --json "$TMP/n1.json"
expect "종료 코드" 0 "$?"
expect "도구: 요청·성공·실패" "2000 2000 0" "$(jget "$TMP/n1.json" "d['result']['requests'], d['result']['ok'], d['result']['failed']" | tr -d '(),')"
expect "프록시가 받은 요청 수" 2000 "$(sget 18581 "d['requests']['client']")"
expect "HIT·MISS: 도구(X-Cache) = 프록시(outcomes)" \
       "$(sget 18581 "d['outcomes']['HIT'], d['outcomes']['MISS']" | tr -d '(),')" \
       "$(jget "$TMP/n1.json" "d['result']['labels']['HIT'], d['result']['labels']['MISS']" | tr -d '(),')"
expect "MISS = 서로 다른 객체 수 = 원서버 요청 수 (빈 캐시, 동시 1)" \
       "$(jget "$TMP/n1.json" "d['result']['distinct']") $(jget "$TMP/n1.json" "d['result']['distinct']")" \
       "$(jget "$TMP/n1.json" "d['result']['labels']['MISS']") $(sget 18581 "d['origin_requests']")"
truth "응답 시간 순서: p50 ≤ p90 ≤ p99 ≤ 최대" \
      "$(jget "$TMP/n1.json" "(lambda l: l['p50'] <= l['p90'] <= l['p99'] <= l['max'])(d['result']['latency_ms'])")"

echo "[2] 요청 5000개, 동시 16 (프록시 A): 동시에 보내도 하나도 빠지거나 더 세지 않음"
before=$(sget 18581 "d['requests']['client']")
hm0=$(sget 18581 "d['outcomes']['HIT'] + d['outcomes']['MISS']")
lc n2 -x 127.0.0.1:18581 -n 5000 -c 16 --json "$TMP/n2.json"
expect "종료 코드" 0 "$?"
expect "프록시가 받은 요청 수 (증가분)" 5000 "$(( $(sget 18581 "d['requests']['client']") - before ))"
expect "도구의 HIT+MISS = 프록시의 HIT+MISS 증가분" \
       "$(( $(sget 18581 "d['outcomes']['HIT'] + d['outcomes']['MISS']") - hm0 ))" \
       "$(jget "$TMP/n2.json" "d['result']['labels']['HIT'] + d['result']['labels']['MISS']")"

echo "[3] 시간 모드: 예열 1초 + 측정 2초, 캐시 미리 채움, 동시 8, 프록시 CPU 측정 (프록시 B, -S 8)"
before=$(sget 18582 "d['requests']['client']")
lc d3 -x 127.0.0.1:18582 -d 2 -w 1 -c 8 --prefill --pid "${PID[B]}" --json "$TMP/d3.json"
expect "종료 코드" 0 "$?"
truth "측정 시간 $(jget "$TMP/d3.json" "round(d['result']['elapsed_sec'], 3)")초 (2초 ± 0.1)" \
      "abs($(jget "$TMP/d3.json" "d['result']['elapsed_sec']") - 2.0) < 0.1"
expect "캐시를 미리 채웠으므로 측정 구간은 모두 HIT (MISS 수)" 0 "$(jget "$TMP/d3.json" "d['result']['labels']['MISS']")"
req=$(jget "$TMP/d3.json" "d['result']['requests']")
delta=$(( $(sget 18582 "d['requests']['client']") - before ))
truth "도구가 센 요청 $req ≤ 프록시가 받은 요청 $delta (예열·미리 채우기는 도구가 세지 않음)" "0 < $req <= $delta"
truth "CPU: 부하 도구 $(jget "$TMP/d3.json" "d['result']['cpu']['loader_cores']")코어, 프록시 $(jget "$TMP/d3.json" "d['result']['cpu']['proxy_cores']")코어, 요청당 프록시 CPU $(jget "$TMP/d3.json" "d['result']['cpu']['proxy_us_per_req']")us (모두 0보다 큼)" \
      "$(jget "$TMP/d3.json" "d['result']['cpu']['loader_cores'] > 0 and d['result']['cpu']['proxy_cores'] > 0 and d['result']['cpu']['proxy_us_per_req'] > 0")"
if grep -q "요청 하나에 프록시 CPU" "$TMP/d3.txt"; then ok "화면 출력에 프록시 CPU 표시"; else ng "화면 출력에 프록시 CPU 없음"; fi

echo "[4] 프록시 2개에 번갈아: 요청 1000개, 동시 4"
a0=$(sget 18581 "d['requests']['client']"); b0=$(sget 18582 "d['requests']['client']")
lc rr -x 127.0.0.1:18581,127.0.0.1:18582 -n 1000 -c 4 --json "$TMP/rr.json"
expect "종료 코드" 0 "$?"
expect "프록시 A·B가 받은 요청 수" "500 500" \
       "$(( $(sget 18581 "d['requests']['client']") - a0 )) $(( $(sget 18582 "d['requests']['client']") - b0 ))"

echo "[5] 오류 검출"
lc dead -x 127.0.0.1:18589 -n 50 -c 4 --json "$TMP/dead.json"
expect "아무도 듣지 않는 포트: 종료 코드 1, 연결 실패 수" "1 50" "$? $(jget "$TMP/dead.json" "d['result']['errors']['connect']")"
lc short -x 127.0.0.1:19581 -n 20 -c 2 --json "$TMP/short.json"
expect "Content-Length보다 짧은 본문: 종료 코드 1, 본문 잘림 수" "1 20" "$? $(jget "$TMP/short.json" "d['result']['errors']['short_body']")"
lc nf -x 127.0.0.1:19582 -n 20 -c 2 --json "$TMP/nf.json"
expect "404 응답: 종료 코드 1, 상태 코드별 개수" "1 {'404': 20}" "$? $(jget "$TMP/nf.json" "d['result']['status']")"
lc pf -x 127.0.0.1:18589 -n 10 --prefill
expect "캐시 채우기 실패 → 종료 코드 2 (측정하지 않음)" 2 "$?"

echo "[6] 옵션 검사 (종료 코드 2)"
lc o1 -n 10 -d 1;            r1=$?
lc o2 -n 10 -w 1;            r2=$?
lc o3 -x nohost:abc -n 1;    r3=$?
lc o4 -c 0 -n 1;             r4=$?
expect "-n과 -d 함께 / -n과 -w 함께 / 잘못된 주소 / -c 0" "2 2 2 2" "$r1 $r2 $r3 $r4"

echo "[7] 정상 종료, 새니타이저"
stop_proc A
stop_proc B
expect "프록시 종료 코드" "00" "$RCS"
reports=$(cat "$LOGDIR"/test6_*.log | grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:')
expect "새니타이저 경고 수 (프록시와 부하 도구 모두, MODE=$MODE)" 0 "$reports"

echo
echo "결과: 통과 $pass개, 실패 $fail개   (로그: $LOGDIR/test6_*.log)"
[ "$fail" -eq 0 ]

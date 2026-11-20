#!/usr/bin/env bash
# 3단계(미니 CDN: 협력형 캐시 클러스터) 완료 기준 자동 점검
#
# 시험용 클러스터: node1·node2·node3 (포트 18281~18283)
#   노드 이름이 conf/cluster.conf와 같으므로 담당 노드도 같다 (담당은 이름으로 계산).
# 기대하는 담당 노드는 rendezvous_test(같은 router.c)로 계산해서 비교한다.
set -u
cd "$(dirname "$0")/.." || exit 1

MODE=${MODE:-debug}
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
ORIGIN_PORT=19200
CAPTURE_PORT=19299      # 원서버가 받는 요청을 그대로 저장하는 시험용 서버
O="http://127.0.0.1:$ORIGIN_PORT"

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

pass=0
fail=0
ok() { printf '  \033[32m✓\033[0m %s\n' "$1"; pass=$((pass + 1)); }
ng() { printf '  \033[31m✗\033[0m %s\n' "$1"; fail=$((fail + 1)); }
expect() {      # 설명, 기대값, 실제값
    if [ "$3" = "$2" ]; then ok "$1 → $3"; else ng "$1 → ${3:-없음} (기대값 $2)"; fi
}
log_has() {     # log_has 노드 문자열 설명
    if grep -qF -- "$2" "$LOGDIR/test3_$1.log"; then ok "$3"; else ng "$3 (기록에 '$2' 없음)"; fi
}

# ---------------------------------------------------------------- 준비
echo "[준비] 빌드 (MODE=$MODE)"
make -s MODE="$MODE" all rtest || { echo "빌드 실패"; exit 1; }
BIN="build/$MODE/proxy"
RT="build/$MODE/rendezvous_test"

if [ ! -f "$WWW/big.bin" ]; then
    echo "[준비] 테스트 파일 생성 → $WWW"
    python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
fi
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/test3_*.log
TMP=$(mktemp -d)

CONF="$TMP/cluster.conf"
printf 'node1 127.0.0.1 18281\nnode2 127.0.0.1 18282\nnode3 127.0.0.1 18283\n' > "$CONF"
declare -A NPORT=([node1]=18281 [node2]=18282 [node3]=18283)    # 클러스터 노드
declare -A IPORT=([node1]=18291 [node2]=18292 [node3]=18293)    # 비교용 독립 프록시
declare -A PID=()
RCS=""                  # 종료한 프록시들의 종료 코드를 이어 붙인 문자열
EXTRA_PIDS=""           # 원서버·시험용 서버

wait_port() {
    for _ in $(seq 1 100); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "  포트 $1 이 열리지 않았습니다. $LOGDIR/test3_*.log 를 확인하세요."
    return 1
}
start_node() {      # start_node 이름 → 클러스터 노드 실행 (실패 노드 건너뛰는 시간 3초)
    "$BIN" -n "$1" -C "$CONF" -r 3000 >> "$LOGDIR/test3_$1.log" 2>&1 &
    PID[$1]=$!
}
stop_node() {       # stop_node 이름 → SIGTERM으로 정상 종료시키고 종료 코드를 모은다
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

rank_of() {         # rank_of 노드목록 URL → "1순위 2순위 3순위"
    "$RT" rank -c "$1" "$2" | awk '$1 ~ /순위$/ { printf "%s ", $2 }'
}
chain() {           # chain 포트 URL [curl 옵션...] → "FORWARD node1;MISS node2;"
    local port=$1 url=$2
    shift 2         # 본문은 $TMP/body, 걸린 시간은 $TMP/time
    curl -s -D "$TMP/head" -o "$TMP/body" -w '%{time_total}' \
         -x "http://127.0.0.1:$port" "$@" "$url" > "$TMP/time"
    tr -d '\r' < "$TMP/head" | awk 'tolower($1) == "x-cache:" { printf "%s %s;", $2, $4 }'
}
want() {            # want 진입노드 처리노드 결과 → 기대하는 X-Cache 경로
    if [ "$1" = "$2" ]; then echo "$3 $2;"; else echo "FORWARD $1;$3 $2;"; fi
}
origin_count() {
    curl -s "$O/__count" | python3 -c "import json,sys; print(json.load(sys.stdin)['$1'])"
}

python3 tools/origin.py --port $ORIGIN_PORT --dir "$WWW" --quiet > "$LOGDIR/test3_origin.log" 2>&1 &
EXTRA_PIDS="$EXTRA_PIDS $!"
for n in node1 node2 node3; do start_node "$n"; done
wait_port $ORIGIN_PORT || exit 1
for n in node1 node2 node3; do wait_port "${NPORT[$n]}" || exit 1; done

# ---------------------------------------------------------------- 점검
echo "[0] 랑데부 해싱 단위 테스트 (rendezvous_test)"
if "$RT" > "$TMP/rt.txt" 2>&1; then
    ok "결정성·분포·노드 제거·노드 추가 검사 모두 통과"
else
    ng "rendezvous_test 실패"
    sed 's/^/      /' "$TMP/rt.txt"
fi
grep '담당이 바뀐' "$TMP/rt.txt" | sed 's/^ */      /'

echo "[1] 담당 노드 일치와 단일 사본: URL 30개를 node1 → node2 → node3 순서로 한 바퀴씩"
curl -s "$O/__reset" > /dev/null
declare -A OWNER
own_cnt=""
for i in $(seq 0 29); do
    u=$(printf '%s/obj/%04d.bin' "$O" "$i")
    read -r -a r <<< "$(rank_of "$CONF" "$u")"
    OWNER[$u]=${r[0]}
    own_cnt="$own_cnt ${r[0]}"
done
bad=0
badbody=0
round=0
for entry in node1 node2 node3; do
    round=$((round + 1))
    res=HIT
    [ $round -eq 1 ] && res=MISS
    for i in $(seq 0 29); do
        f=$(printf 'obj/%04d.bin' "$i")
        u="$O/$f"
        got=$(chain "${NPORT[$entry]}" "$u")
        exp=$(want "$entry" "${OWNER[$u]}" "$res")
        if [ "$got" != "$exp" ]; then
            bad=$((bad + 1))
            [ $bad -le 3 ] && echo "      예: $entry로 $f → $got (기대 $exp)"
        fi
        cmp -s "$TMP/body" "$WWW/$f" || badbody=$((badbody + 1))
    done
done
echo "      담당 분포: node1 $(grep -o node1 <<< "$own_cnt" | wc -l)개," \
     "node2 $(grep -o node2 <<< "$own_cnt" | wc -l)개, node3 $(grep -o node3 <<< "$own_cnt" | wc -l)개"
expect "X-Cache 경로가 담당 노드 계산과 다른 응답 (90개 중)" 0 "$bad"
expect "본문이 원본과 다른 응답 (90개 중)" 0 "$badbody"
expect "원서버가 받은 요청 수 (클러스터: URL마다 한 번, 담당 노드에만 저장)" 30 "$(origin_count total)"

echo "      비교: 같은 요청을 독립 프록시 3개(클러스터 설정 없음)에 보내면"
declare -A IPID=()
for n in node1 node2 node3; do
    "$BIN" -n "$n" -p "${IPORT[$n]}" -q >> "$LOGDIR/test3_indep_$n.log" 2>&1 &
    IPID[$n]=$!
done
for n in node1 node2 node3; do wait_port "${IPORT[$n]}" || exit 1; done
curl -s "$O/__reset" > /dev/null
for entry in node1 node2 node3; do
    for i in $(seq 0 29); do
        curl -s -o /dev/null -x "http://127.0.0.1:${IPORT[$entry]}" "$(printf '%s/obj/%04d.bin' "$O" "$i")"
    done
done
expect "원서버가 받은 요청 수 (독립: 노드마다 따로 받아 따로 저장)" 90 "$(origin_count total)"
for n in node1 node2 node3; do
    kill -TERM "${IPID[$n]}"
    wait "${IPID[$n]}"
    RCS="$RCS$?"
done

echo "[2] Via 헤더: 루프 방지와 경로 기록"
u="$O/obj/0100.bin"
read -r -a r <<< "$(rank_of "$CONF" "$u")"
expect "담당이 아닌 ${r[2]}에 'Via: 1.1 node1'을 붙여 요청 → 다시 넘기지 않고 ${r[2]}에서 처리" \
       "MISS ${r[2]};" "$(chain "${NPORT[${r[2]}]}" "$u" -H 'Via: 1.1 node1')"
u="$O/obj/0101.bin"
read -r -a r <<< "$(rank_of "$CONF" "$u")"
expect "클러스터 밖 프록시의 Via(1.1 corp-proxy)는 무시 → 담당 ${r[0]}에 전달" \
       "FORWARD ${r[2]};MISS ${r[0]};" "$(chain "${NPORT[${r[2]}]}" "$u" -H 'Via: 1.1 corp-proxy')"

# 원서버 자리에 "받은 요청을 파일로 저장하고 짧은 응답을 주는" 시험용 서버를 둔다
python3 - "$CAPTURE_PORT" "$TMP/captured" > /dev/null 2>&1 <<'EOF' &
import socket, sys
port, out = int(sys.argv[1]), sys.argv[2]
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(8)
while True:
    c, _ = s.accept()
    c.settimeout(5)
    data = b""
    try:
        while b"\r\n\r\n" not in data:
            chunk = c.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    if not data:            # 포트 확인용 연결(nc -z)은 무시
        c.close()
        continue
    open(out, "wb").write(data)
    c.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok")
    c.close()
    break
EOF
EXTRA_PIDS="$EXTRA_PIDS $!"
wait_port $CAPTURE_PORT || exit 1
u="http://127.0.0.1:$CAPTURE_PORT/via-check"
read -r -a r <<< "$(rank_of "$CONF" "$u")"
chain "${NPORT[${r[2]}]}" "$u" -H 'Via: 1.1 corp-proxy' > /dev/null
expect "원서버가 받은 요청 줄 (절대 URL이 아닌 경로만)" "GET /via-check HTTP/1.0" \
       "$(head -1 "$TMP/captured" 2>/dev/null | tr -d '\r')"
expect "원서버가 받은 Via (클라이언트 → ${r[2]} → ${r[0]} 순서로 덧붙음)" \
       "Via: 1.1 corp-proxy, 1.1 ${r[2]}, 1.0 ${r[0]}" \
       "$(grep -i '^Via:' "$TMP/captured" 2>/dev/null | tr -d '\r')"

echo "[3] 담당 노드 장애 → 2순위 노드가 대신 처리 → 재시작 후 복구"
u="$O/obj/0110.bin"
read -r -a r <<< "$(rank_of "$CONF" "$u")"
OW=${r[0]}; S=${r[1]}; T=${r[2]}
echo "      obj/0110.bin 순위: $OW(담당) > $S(대체) > $T"
expect "$T에 요청" "FORWARD $T;MISS $OW;" "$(chain "${NPORT[$T]}" "$u")"
stop_node "$OW"
expect "$OW 종료 후 $T에 요청 → 대체 노드 $S에서 받아 저장" "FORWARD $T;MISS $S;" "$(chain "${NPORT[$T]}" "$u")"
expect "다시 $T에 요청 → $OW를 건너뛰고 $S의 캐시에서" "FORWARD $T;HIT $S;" "$(chain "${NPORT[$T]}" "$u")"
log_has "$T" "$OW(127.0.0.1:${NPORT[$OW]})에 요청을 넘길 수 없습니다" "$T의 기록: $OW 연결 불가 → 일정 시간 건너뜀"
expect "$S에 직접 요청 → $S에서 대체 담당으로 처리" "HIT $S;" "$(chain "${NPORT[$S]}" "$u")"
log_has "$S" "대체 ($OW 연결 불가)" "$S의 기록: 대체 담당으로 처리"
start_node "$OW"
wait_port "${NPORT[$OW]}" || exit 1
expect "$OW 재시작 직후 (건너뛰는 3초 이내) → 아직 $S에서 처리" "FORWARD $T;HIT $S;" "$(chain "${NPORT[$T]}" "$u")"
sleep 3.2
expect "3초 뒤 → $OW를 다시 시도해 복구 (재시작으로 캐시가 비었으므로 MISS)" \
       "FORWARD $T;MISS $OW;" "$(chain "${NPORT[$T]}" "$u")"
log_has "$T" "$OW 복구: 다시 요청을 넘깁니다." "$T의 기록: $OW 복구"

echo "[4] 1·2순위 노드가 모두 장애 → 남은 노드가 직접 처리"
u="$O/obj/0120.bin"
read -r -a r <<< "$(rank_of "$CONF" "$u")"
OW=${r[0]}; S=${r[1]}; T=${r[2]}
echo "      obj/0120.bin 순위: $OW > $S > $T"
stop_node "$OW"
stop_node "$S"
expect "$OW·$S 종료 후 $T에 요청 → $T에서 원서버로 직접" "MISS $T;" "$(chain "${NPORT[$T]}" "$u")"
log_has "$T" "직접 ($OW,$S 연결 불가)" "$T의 기록: 직접 처리"
start_node "$OW"
start_node "$S"
wait_port "${NPORT[$OW]}" || exit 1
wait_port "${NPORT[$S]}" || exit 1

echo "[5] 응답하지 않는 노드 (연결 시간 초과) → 대체, 그 뒤로는 기다리지 않고 건너뜀"
CONF2="$TMP/cluster2.conf"
printf 'tx 127.0.0.1 18295\nty 127.0.0.1 18296\n' > "$CONF2"
# ty: 연결 대기열(backlog 0)을 한 개로 채워 두면, 이후 연결 요청(SYN)을 커널이 버린다
python3 - > "$TMP/blackhole" 2>&1 <<'EOF' &
import socket, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 18296))
s.listen(0)
filler = socket.create_connection(("127.0.0.1", 18296))
print("ready", flush=True)
time.sleep(600)
EOF
EXTRA_PIDS="$EXTRA_PIDS $!"
for _ in $(seq 1 50); do grep -q ready "$TMP/blackhole" && break; sleep 0.1; done
"$BIN" -n tx -C "$CONF2" -c 400 -r 3000 >> "$LOGDIR/test3_tx.log" 2>&1 &
PID[tx]=$!
wait_port 18295 || exit 1
W=()
for i in $(seq 0 99); do
    u=$(printf '%s/obj/%04d.bin' "$O" "$i")
    read -r -a r <<< "$(rank_of "$CONF2" "$u")"
    [ "${r[0]}" = ty ] && W+=("$u")
    [ ${#W[@]} -ge 2 ] && break
done
x=$(chain 18295 "${W[0]}"); t=$(cat "$TMP/time")
expect "ty가 담당인 URL → 2순위 tx에서 직접 처리" "MISS tx;" "$x"
if awk -v t="$t" 'BEGIN { exit !(t >= 0.4) }'; then
    ok "첫 요청은 연결 제한 시간(0.4초)을 기다린 뒤 대체 (${t}초)"
else
    ng "첫 요청 ${t}초 (0.4초 이상이어야 함)"
fi
x=$(chain 18295 "${W[1]}"); t=$(cat "$TMP/time")
expect "ty가 담당인 다른 URL" "MISS tx;" "$x"
if awk -v t="$t" 'BEGIN { exit !(t < 0.3) }'; then
    ok "두 번째 요청은 ty를 건너뛰어 기다리지 않음 (${t}초)"
else
    ng "두 번째 요청 ${t}초 (0.3초 미만이어야 함)"
fi
log_has tx "ty(127.0.0.1:18296)에 요청을 넘길 수 없습니다" "tx의 기록: ty 연결 시간 초과 → 건너뜀"
stop_node tx

# 장애 표시는 여러 스레드가 동시에 읽고 쓴다. 재시도 간격을 0.1초로 줄이고 동시 요청을 몰아
# "건너뛸지 확인(읽기)"과 "다시 시도 후 실패 표시(쓰기)"가 계속 겹치게 만든다 (TSan 검사용)
echo "      동시 접근: 꺼진 노드 tb + 재시도 간격 0.1초 + 동시 요청 600개 (장애 표시의 경쟁 조건 검사)"
CONF3="$TMP/cluster3.conf"
printf 'ta 127.0.0.1 18297\ntb 127.0.0.1 18298\n' > "$CONF3"     # 18298에는 아무도 없음
"$BIN" -n ta -C "$CONF3" -r 100 -q >> "$LOGDIR/test3_ta.log" 2>&1 &
PID[ta]=$!
wait_port 18297 || exit 1
: > "$TMP/tb_urls"
for i in $(seq 0 99); do
    u=$(printf '%s/obj/%04d.bin' "$O" "$i")
    read -r -a r <<< "$(rank_of "$CONF3" "$u")"
    [ "${r[0]}" = tb ] && echo "$u" >> "$TMP/tb_urls"
    [ "$(wc -l < "$TMP/tb_urls")" -ge 8 ] && break
done
seq 0 599 | xargs -P 16 -I{} sh -c '
    u=$(sed -n "$(( {} % 8 + 1 ))p" '"$TMP/tb_urls"')
    curl -s -o /dev/null -w "%{http_code}\n" -x http://127.0.0.1:18297 "$u"
' > "$TMP/tb_codes"
expect "tb가 담당인 URL 8개에 동시 요청 600개 → 200 응답 수" 600 "$(grep -c '^200$' "$TMP/tb_codes")"
stop_node ta
probes=$(grep -o '다른 노드 연결 실패 [0-9]*' "$LOGDIR/test3_ta.log" | awk '{print $NF}')
if [ "${probes:-0}" -ge 2 ]; then
    ok "부하 중 tb 재시도·실패 표시 ${probes}회 (읽기와 쓰기가 겹친 상황)"
else
    ng "tb 재시도가 ${probes:-0}회뿐 (2회 이상이어야 함)"
fi

echo "[6] 부하 중 노드 종료: 요청 1200개(URL 60개, 동시 16개)를 node1·node3로 보내는 도중 node2 종료"
seq 0 1199 | xargs -P 16 -I{} sh -c '
    i={}
    p=$(printf "obj/%04d.bin" $(( 130 + i % 60 )))
    port=$(( i % 2 == 0 ? 18281 : 18283 ))
    curl -s -o /dev/null -x http://127.0.0.1:$port -w "$p %{http_code} %{size_download}\n" '"$O"'/$p
' > "$TMP/load.txt" &
LOAD=$!
sleep 1
stop_node node2
wait $LOAD
bad=$(python3 - "$WWW" "$TMP/load.txt" <<'EOF'
import os, sys
www, path = sys.argv[1], sys.argv[2]
lines = [l for l in open(path).read().split("\n") if l]
bad = sum(1 for l in lines
          if l.split()[1] != "200" or int(l.split()[2]) != os.path.getsize(os.path.join(www, l.split()[0])))
print(bad if len(lines) == 1200 else f"응답 {len(lines)}개")
EOF
)
expect "실패하거나 크기가 틀린 응답 (1200개 중)" 0 "$bad"
n=$(cat "$LOGDIR/test3_node1.log" "$LOGDIR/test3_node3.log" | grep -c "node2 연결 불가")
if [ "$n" -gt 0 ]; then
    ok "부하 도중 node2 장애를 감지하고 대체한 요청 ${n}개"
else
    ng "대체가 일어나지 않음 (node2가 부하가 끝난 뒤에 종료됨)"
fi

echo "[7] 정상 종료, 통계, 새니타이저"
for n in "${!PID[@]}"; do stop_node "$n"; done
if [[ "$RCS" =~ ^0+$ ]]; then
    ok "프록시 ${#RCS}개 모두 종료 코드 0"
else
    ng "종료 코드: $RCS (모두 0이어야 함)"
fi
log_has node1 "클러스터: 넘긴 요청" "종료 시 클러스터 통계 출력"
reports=$(cat "$LOGDIR"/test3_*.log | grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:')
expect "새니타이저 경고 수 (MODE=$MODE)" 0 "$reports"

echo
echo "결과: 통과 $pass개, 실패 $fail개   (로그: $LOGDIR/test3_*.log)"
[ "$fail" -eq 0 ]

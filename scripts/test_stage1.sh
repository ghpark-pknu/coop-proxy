#!/usr/bin/env bash
# 1단계(중계 프록시) 완료 기준 자동 점검
# 손으로 띄운 프록시(8080)·원서버(9000)와 겹치지 않도록 다른 포트를 쓴다.
set -u
cd "$(dirname "$0")/.." || exit 1

MODE=${MODE:-debug}
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
PROXY_PORT=18080
ORIGIN_PORT=19000
SLOW_PORT=19001          # 3초 늦게 응답하는 원서버 (504 확인용)

# 환경에 다른 프록시 설정이 있으면 curl이 우리 프록시를 건너뛸 수 있으므로 비운다
unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

pass=0
fail=0
ok() { printf '  \033[32m✓\033[0m %s\n' "$1"; pass=$((pass + 1)); }
ng() { printf '  \033[31m✗\033[0m %s\n' "$1"; fail=$((fail + 1)); }

# ---------------------------------------------------------------- 준비
echo "[준비] 빌드 (MODE=$MODE)"
make -s MODE="$MODE" || { echo "빌드 실패"; exit 1; }
BIN="build/$MODE/proxy"

if [ ! -f "$WWW/big.bin" ]; then
    echo "[준비] 테스트 파일 생성 → $WWW"
    python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
fi
mkdir -p "$LOGDIR"
TMP=$(mktemp -d)

python3 tools/origin.py --port $ORIGIN_PORT --dir "$WWW" --quiet > "$LOGDIR/test_origin.log" 2>&1 &
ORIGIN_PID=$!
python3 tools/origin.py --port $SLOW_PORT --dir "$WWW" --quiet --delay 3 > /dev/null 2>&1 &
SLOW_PID=$!
# 송수신 제한 1.5초 → 3초 늦는 원서버는 504가 되어야 한다
# -s 0: 캐시를 끈다 (2단계부터 캐시가 기본으로 켜지므로, 순수 중계 기능만 점검)
"$BIN" -p $PROXY_PORT -t 1500 -s 0 > "$LOGDIR/test_proxy.log" 2>&1 &
PROXY_PID=$!

cleanup() {
    kill "$ORIGIN_PID" "$SLOW_PID" 2>/dev/null
    kill -INT "$PROXY_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

wait_port() {
    for _ in $(seq 1 50); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
for p in $ORIGIN_PORT $SLOW_PORT $PROXY_PORT; do
    wait_port "$p" || { echo "포트 $p 가 열리지 않았습니다. $LOGDIR 의 로그를 확인하세요."; exit 1; }
done

X="-x http://127.0.0.1:$PROXY_PORT"
O="http://127.0.0.1:$ORIGIN_PORT"
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
raw()  { printf "$1" | nc -w 3 127.0.0.1 $PROXY_PORT | head -1 | tr -d '\r' | cut -d' ' -f2; }
expect() {      # 설명, 기대값, 실제값
    if [ "$3" = "$2" ]; then ok "$1 → $3"; else ng "$1 → ${3:-응답 없음} (기대값 $2)"; fi
}

# ---------------------------------------------------------------- 점검
echo "[1] 파일 중계: 프록시로 받은 파일이 원본과 바이트 단위로 같은가"
for f in small.txt page.html big.bin obj/0000.bin; do
    if curl -s $X "$O/$f" -o "$TMP/out" && cmp -s "$TMP/out" "$WWW/$f"; then
        ok "$f ($(stat -c %s "$WWW/$f") 바이트)"
    else
        ng "$f"
    fi
done

echo "[2] 요청이 실제로 원서버까지 가는가"
curl -s "$O/__reset" > /dev/null
for _ in 1 2 3; do curl -s $X "$O/small.txt" -o /dev/null; done
total=$(curl -s "$O/__count" | python3 -c 'import json,sys; print(json.load(sys.stdin)["total"])')
expect "프록시로 3번 요청 → 원서버 요청 수 (캐시가 없으므로 3)" 3 "$total"

echo "[3] 오류 응답"
expect "형식이 틀린 요청"          400 "$(raw 'HELLO\r\n\r\n')"
expect "잘못된 URL (호스트 없음)"  400 "$(raw 'GET http:///x HTTP/1.1\r\n\r\n')"
expect "POST 메서드"               501 "$(code $X -X POST "$O/small.txt")"
expect "CONNECT (HTTPS 터널)"      501 "$(curl -s -o /dev/null -w '%{http_connect}' $X https://example.com/)"
expect "없는 관제 경로 /__nope"    404 "$(code "http://127.0.0.1:$PROXY_PORT/__nope")"
expect "닫힌 포트"                 502 "$(code $X http://127.0.0.1:1/)"
expect "존재하지 않는 호스트"      502 "$(code $X http://nonexistent.invalid/)"
expect "응답이 3초 늦는 원서버"    504 "$(code $X "http://127.0.0.1:$SLOW_PORT/small.txt")"

echo "[4] 동시 요청 (ab: 요청 2000개, 동시 연결 50개)"
res=$(ab -q -n 2000 -c 50 -X 127.0.0.1:$PROXY_PORT "$O/small.txt" 2>&1)
complete=$(echo "$res" | awk '/^Complete requests/ {print $3}')
failed=$(echo "$res" | awk '/^Failed requests/ {print $3}')
non2xx=$(echo "$res" | awk '/^Non-2xx responses/ {print $3}')
rps=$(echo "$res" | awk '/^Requests per second/ {print $4}')
if [ "${complete:-0}" = 2000 ] && [ "${failed:-1}" = 0 ] && [ -z "$non2xx" ]; then
    ok "2000개 완료, 실패 0 (초당 ${rps} 요청)"
else
    ng "완료 ${complete:-?}, 실패 ${failed:-?}, 2xx가 아닌 응답 ${non2xx:-0}"
fi

echo "[5] 정상 종료와 새니타이저 보고"
kill -INT "$PROXY_PID"
wait "$PROXY_PID"
rc=$?
expect "SIGINT(Ctrl+C)로 종료 → 종료 코드" 0 "$rc"
reports=$(grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:' \
          "$LOGDIR/test_proxy.log")
expect "새니타이저 경고 수 (MODE=$MODE)" 0 "$reports"

echo
echo "결과: 통과 $pass개, 실패 $fail개   (로그: $LOGDIR/test_proxy.log)"
[ "$fail" -eq 0 ]

#!/usr/bin/env bash
# 2단계(캐시) 완료 기준 자동 점검
#
# 사용법:  ./scripts/test_stage2.sh              디버그 빌드로 점검
#          MODE=tsan ./scripts/test_stage2.sh    데이터 경쟁 검사 빌드로 점검
#          MODE=asan ./scripts/test_stage2.sh    메모리 오류 검사 빌드로 점검
#
# 프록시를 설정만 바꿔 네 개 띄운다.
#   A 18180  기본 설정 + 차단 목록         : HIT, 객체 상한, 차단, 클라이언트 조건부 헤더
#   B 18181  용량 40KB, 객체 상한 20KB     : LRU 교체 순서
#   C 18182  검증 주기 1초                 : 304(VALIDATED), 200(REFRESH), STALE
#   D 18183  용량 256KB, 객체 상한 64KB    : 동시 요청 + 잦은 교체 (경쟁 조건 검사)
set -u
cd "$(dirname "$0")/.." || exit 1

MODE=${MODE:-debug}
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
ORIGIN_PORT=19100
TEMP_ORIGIN_PORT=19102    # STALE 확인용: 도중에 꺼 버릴 원서버

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

pass=0
fail=0
ok() { printf '  \033[32m✓\033[0m %s\n' "$1"; pass=$((pass + 1)); }
ng() { printf '  \033[31m✗\033[0m %s\n' "$1"; fail=$((fail + 1)); }
expect() {      # 설명, 기대값, 실제값
    if [ "$3" = "$2" ]; then ok "$1 → $3"; else ng "$1 → ${3:-없음} (기대값 $2)"; fi
}

# ---------------------------------------------------------------- 준비
echo "[준비] 빌드 (MODE=$MODE)"
make -s MODE="$MODE" || { echo "빌드 실패"; exit 1; }
BIN="build/$MODE/proxy"

if [ ! -f "$WWW/big.bin" ]; then
    echo "[준비] 테스트 파일 생성 → $WWW"
    python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
fi
# 이 점검 전용 파일 (내용을 바꾸는 시험을 하므로 따로 둔다)
mkdir -p "$WWW/t2" "$LOGDIR"
for f in a b c; do head -c 16384 /dev/urandom > "$WWW/t2/$f.bin"; done
echo "v1" > "$WWW/t2/change.txt"
TMP=$(mktemp -d)

python3 tools/origin.py --port $ORIGIN_PORT --dir "$WWW" --quiet > "$LOGDIR/test2_origin.log" 2>&1 &
ORIGIN_PID=$!
python3 tools/origin.py --port $TEMP_ORIGIN_PORT --dir "$WWW" --quiet > /dev/null 2>&1 &
TEMP_PID=$!
"$BIN" -p 18180 -n A -b conf/blocklist.txt > "$LOGDIR/test2_proxyA.log" 2>&1 &  PA=$!
"$BIN" -p 18181 -n B -s 40K -o 20K         > "$LOGDIR/test2_proxyB.log" 2>&1 &  PB=$!
"$BIN" -p 18182 -n C -v 1                  > "$LOGDIR/test2_proxyC.log" 2>&1 &  PC=$!
"$BIN" -p 18183 -n D -s 256K -o 64K -q     > "$LOGDIR/test2_proxyD.log" 2>&1 &  PD=$!

cleanup() {
    kill "$ORIGIN_PID" "$TEMP_PID" 2>/dev/null
    kill -INT "$PA" "$PB" "$PC" "$PD" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP" "$WWW/t2"
}
trap cleanup EXIT

wait_port() {
    for _ in $(seq 1 50); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
for p in $ORIGIN_PORT $TEMP_ORIGIN_PORT 18180 18181 18182 18183; do
    wait_port "$p" || { echo "포트 $p 가 열리지 않았습니다. $LOGDIR 의 로그를 확인하세요."; exit 1; }
done

O="http://127.0.0.1:$ORIGIN_PORT"

# 프록시(포트)를 거쳐 받고, 응답의 X-Cache 값(HIT, MISS, ...)을 출력한다. 본문은 $TMP/body
get() {     # get 프록시포트 URL [curl 추가 옵션...]
    local port=$1 url=$2
    shift 2
    curl -s -D "$TMP/head" -o "$TMP/body" -x "http://127.0.0.1:$port" "$@" "$url"
    awk 'tolower($1) == "x-cache:" { print $2 }' "$TMP/head" | tr -d '\r'
}
origin_count() {    # 원서버가 받은 요청 수: total 또는 conditional
    curl -s "$O/__count" | python3 -c "import json,sys; print(json.load(sys.stdin)['$1'])"
}

# ---------------------------------------------------------------- 점검
echo "[1] 캐시 적중 (프록시 A)"
curl -s "$O/__reset" > /dev/null
expect "small.txt 첫 요청"   MISS "$(get 18180 "$O/small.txt")"
expect "small.txt 두 번째"   HIT  "$(get 18180 "$O/small.txt")"
cmp -s "$TMP/body" "$WWW/small.txt" && ok "캐시로 받은 본문이 원본과 같음" || ng "캐시 본문이 원본과 다름"
expect "원서버가 받은 요청 수 (두 번째는 캐시가 응답)" 1 "$(origin_count total)"

echo "[2] 객체 크기 상한 (기본 1MB, big.bin = 5MB)"
curl -s "$O/__reset" > /dev/null
expect "big.bin 첫 요청"     MISS "$(get 18180 "$O/big.bin")"
expect "big.bin 두 번째"     MISS "$(get 18180 "$O/big.bin")"
expect "원서버 요청 수 (상한 초과라 저장 안 함)" 2 "$(origin_count total)"

echo "[3] LRU 교체 순서 (프록시 B: 용량 40KB, 16KB 파일 3개)"
expect "a 요청"                                  MISS "$(get 18181 "$O/t2/a.bin")"
expect "b 요청"                                  MISS "$(get 18181 "$O/t2/b.bin")"
expect "a 다시 → a가 가장 최근에 쓴 객체가 됨"   HIT  "$(get 18181 "$O/t2/a.bin")"
expect "c 요청 → 용량 초과, 가장 오래 안 쓴 b를 내보냄" MISS "$(get 18181 "$O/t2/c.bin")"
expect "a 다시 → 남아 있음 (FIFO였다면 a가 쫓겨났을 것)" HIT "$(get 18181 "$O/t2/a.bin")"
expect "b 다시 → 쫓겨났으므로 MISS"              MISS "$(get 18181 "$O/t2/b.bin")"

echo "[4] 캐시 검증: 조건부 GET (프록시 C: 검증 주기 1초)"
curl -s "$O/__reset" > /dev/null
expect "change.txt 첫 요청"                      MISS "$(get 18182 "$O/t2/change.txt")"
expect "바로 다시 (검증 주기 이내)"              HIT  "$(get 18182 "$O/t2/change.txt")"
sleep 1.2
expect "1.2초 뒤 → If-Modified-Since → 304"      VALIDATED "$(get 18182 "$O/t2/change.txt")"
expect "본문" v1 "$(cat "$TMP/body")"
expect "원서버가 받은 조건부 요청 수"            1 "$(origin_count conditional)"
echo "v2" > "$WWW/t2/change.txt"
touch -d "@$(( $(date +%s) + 5 ))" "$WWW/t2/change.txt"     # 수정 시각을 확실히 뒤로
sleep 1.2
expect "파일 수정 후 → 200 → 새 내용으로 교체"   REFRESH "$(get 18182 "$O/t2/change.txt")"
expect "본문" v2 "$(cat "$TMP/body")"
expect "바로 다시 → 새 내용이 캐시에서"          HIT "$(get 18182 "$O/t2/change.txt")"
expect "본문" v2 "$(cat "$TMP/body")"

TO="http://127.0.0.1:$TEMP_ORIGIN_PORT"
expect "임시 원서버의 a.bin 요청"                MISS "$(get 18182 "$TO/t2/a.bin")"
kill "$TEMP_PID"; wait "$TEMP_PID" 2>/dev/null
sleep 1.2
expect "원서버를 끈 뒤 검증 시도 → 오래된 사본으로 응답" STALE "$(get 18182 "$TO/t2/a.bin")"
cmp -s "$TMP/body" "$WWW/t2/a.bin" && ok "STALE 응답 본문이 원본과 같음" || ng "STALE 응답 본문이 다름"

echo "[5] 사이트 차단 (프록시 A, conf/blocklist.txt)"
code() { curl -s -o /dev/null -w '%{http_code}' -x http://127.0.0.1:18180 "$1"; }
expect "blocked.test"                     403 "$(code http://blocked.test/)"
expect "www.blocked.test (하위 도메인)"   403 "$(code http://www.blocked.test/)"
expect "notblocked.test (이름만 비슷함)"  502 "$(code http://notblocked.test/)"
expect "차단 목록에 없는 원서버"          200 "$(code "$O/page.html")"

echo "[6] 클라이언트의 조건부 헤더는 원서버로 넘기지 않음 (프록시 A)"
x=$(get 18180 "$O/obj/0001.bin" -H 'If-Modified-Since: Wed, 01 Jan 2031 00:00:00 GMT')
expect "미래 날짜 If-Modified-Since → 304가 아닌 전체 응답" 200 \
       "$(awk 'NR == 1 { print $2 }' "$TMP/head")"
expect "X-Cache" MISS "$x"
expect "다시 요청 → 저장되어 있음" HIT "$(get 18180 "$O/obj/0001.bin")"

echo "[7] 동시 요청 (경쟁 조건 검사)"
# 프록시 D: 작은 캐시에 객체 40개를 16개 동시 연결로 600번 요청 → 저장·교체·적중이 동시에 일어남
seq 0 599 | xargs -P 16 -I{} sh -c '
    p=$(printf "obj/%04d.bin" $(( {} % 40 )))
    curl -s -o /dev/null -x http://127.0.0.1:18183 -w "$p %{http_code} %{size_download}\n" '"$O"'/$p
' > "$TMP/stress.txt"
bad=$(python3 - "$WWW" "$TMP/stress.txt" <<'EOF'
import os, sys
www, path = sys.argv[1], sys.argv[2]
bad = 0
lines = open(path).read().split("\n")
for line in filter(None, lines):
    p, code, size = line.split()
    if code != "200" or int(size) != os.path.getsize(os.path.join(www, p)):
        bad += 1
print(bad if len(list(filter(None, lines))) == 600 else "count")
EOF
)
expect "600개 응답 중 상태·크기가 틀린 것" 0 "$bad"

res=$(ab -q -n 3000 -c 50 -X 127.0.0.1:18180 "$O/small.txt" 2>&1)
complete=$(echo "$res" | awk '/^Complete requests/ {print $3}')
failed=$(echo "$res" | awk '/^Failed requests/ {print $3}')
rps=$(echo "$res" | awk '/^Requests per second/ {print $4}')
if [ "${complete:-0}" = 3000 ] && [ "${failed:-1}" = 0 ]; then
    ok "같은 객체에 동시 HIT 3000개 (초당 ${rps} 요청)"
else
    ng "ab: 완료 ${complete:-?}, 실패 ${failed:-?}"
fi

echo "[8] 정상 종료, 통계, 새니타이저"
rcs=""
for pid in $PA $PB $PC $PD; do
    kill -INT "$pid"
    wait "$pid"
    rcs="$rcs$?"
done
expect "프록시 4개의 종료 코드" 0000 "$rcs"
evict=$(grep -o 'LRU 교체 [0-9]*' "$LOGDIR/test2_proxyD.log" | awk '{print $3}')
if [ "${evict:-0}" -gt 0 ]; then ok "프록시 D에서 LRU 교체 ${evict}회 발생 (교체와 동시 접근이 함께 일어남)"; else ng "프록시 D에서 교체가 일어나지 않음"; fi
grep -q '히트율' "$LOGDIR/test2_proxyA.log" && ok "종료 시 통계 출력" || ng "통계 출력 없음"
reports=$(cat "$LOGDIR"/test2_proxy?.log | grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:')
expect "새니타이저 경고 수 (MODE=$MODE)" 0 "$reports"

echo
echo "결과: 통과 $pass개, 실패 $fail개   (로그: $LOGDIR/test2_*.log)"
[ "$fail" -eq 0 ]

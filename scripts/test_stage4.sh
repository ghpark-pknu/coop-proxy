#!/usr/bin/env bash
# 4단계(실시간 관제: /__stats, /__dashboard, Zipf 부하 생성기) 완료 기준 자동 점검
#
# 사용법:  ./scripts/test_stage4.sh              디버그 빌드로 점검
#          MODE=tsan ./scripts/test_stage4.sh    데이터 경쟁 검사 빌드로 점검
#          MODE=asan ./scripts/test_stage4.sh    메모리 오류 검사 빌드로 점검
#
# 시험용 포트: 클러스터 node1·node2·node3 18381~18383, 비교용 독립 프록시 18391~18393,
#             원서버 19300, 관제 화면 파일 찾기 시험 18387·18388, 아무도 듣지 않는 포트 18389
# 노드는 -q(요청 기록 생략)와 -v 600(검증 주기 10분: 시험 도중 재검증이 끼어들지 않게)으로 띄운다.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD

MODE=${MODE:-debug}
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
ORIGIN_PORT=19300
O="127.0.0.1:$ORIGIN_PORT"
DEAD=18389
CL="127.0.0.1:18381,127.0.0.1:18382,127.0.0.1:18383"     # 클러스터 진입 노드
IND="127.0.0.1:18391,127.0.0.1:18392,127.0.0.1:18393"    # 독립 프록시

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

pass=0
fail=0
ok() { printf '  \033[32m✓\033[0m %s\n' "$1"; pass=$((pass + 1)); }
ng() { printf '  \033[31m✗\033[0m %s\n' "$1"; fail=$((fail + 1)); }
expect() {      # 설명, 기대값, 실제값
    if [ "$3" = "$2" ]; then ok "$1 → $3"; else ng "$1 → ${3:-없음} (기대값 $2)"; fi
}
truth() {       # truth 설명 파이썬식 → 식이 참이면 통과 (식 안의 값은 미리 채워 넣는다)
    if python3 -c "import sys; sys.exit(0 if ($2) else 1)" 2>/dev/null; then ok "$1"; else ng "$1"; fi
}
jget() {        # jget 파일 파이썬식 (d = JSON) → 값
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print($2)" "$1" 2>/dev/null
}
sget() {        # sget 포트 파이썬식 (d = /__stats JSON) → 값
    curl -s "http://127.0.0.1:$1/__stats" | python3 -c "import json,sys; d=json.load(sys.stdin); print($2)" 2>/dev/null
}
hdr() {         # hdr 응답헤더파일 이름 → 값 (없으면 빈 문자열)
    tr -d '\r' < "$1" | awk -v n="$(echo "$2" | tr 'A-Z' 'a-z')" 'tolower($1) == n":" { $1 = ""; sub(/^ /, ""); print; exit }'
}

# ---------------------------------------------------------------- 준비
echo "[준비] 빌드 (MODE=$MODE)"
make -s MODE="$MODE" all || { echo "빌드 실패"; exit 1; }
BIN="build/$MODE/proxy"

if [ ! -f "$WWW/manifest.json" ]; then
    echo "[준비] 테스트 파일 생성 → $WWW"
    python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
fi
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/test4_*.log
TMP=$(mktemp -d)

CONF="$TMP/cluster.conf"
printf 'node1 127.0.0.1 18381\nnode2 127.0.0.1 18382\nnode3 127.0.0.1 18383\n' > "$CONF"
declare -A IPORT=([node1]=18391 [node2]=18392 [node3]=18393)
declare -A PID=()
RCS=""                  # 종료한 프록시들의 종료 코드를 이어 붙인 문자열
ORIGIN_PID=""
LG=(python3 tools/loadgen.py --origin "$O" --manifest "$WWW/manifest.json" -q)

wait_port() {
    for _ in $(seq 1 100); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "  포트 $1 이 열리지 않았습니다. $LOGDIR/test4_*.log 를 확인하세요."
    return 1
}
start_cluster() {   # 클러스터 노드 셋을 새로 띄운다 (캐시가 빈 상태)
    for n in node1 node2 node3; do
        "$BIN" -n "$n" -C "$CONF" -q -v 600 -r 3000 >> "$LOGDIR/test4_$n.log" 2>&1 &
        PID[$n]=$!
    done
    for p in 18381 18382 18383; do wait_port "$p" || exit 1; done
}
start_independent() {
    for n in node1 node2 node3; do
        "$BIN" -n "$n" -p "${IPORT[$n]}" -q -v 600 >> "$LOGDIR/test4_i$n.log" 2>&1 &
        PID[i$n]=$!
    done
    for p in 18391 18392 18393; do wait_port "$p" || exit 1; done
}
stop_proc() {       # stop_proc 키 → SIGTERM으로 정상 종료시키고 종료 코드를 모은다
    kill -TERM "${PID[$1]}" 2>/dev/null
    wait "${PID[$1]}"
    RCS="$RCS$?"
    unset "PID[$1]"
}
cleanup() {
    for p in "${PID[@]}" $ORIGIN_PID; do kill -TERM "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

origin_reset() { curl -s "http://$O/__reset" > /dev/null; }
origin_total() { curl -s "http://$O/__count" | python3 -c "import json,sys; print(json.load(sys.stdin)['total'])"; }

python3 tools/origin.py --port $ORIGIN_PORT --dir "$WWW" --quiet > "$LOGDIR/test4_origin.log" 2>&1 &
ORIGIN_PID=$!
wait_port $ORIGIN_PORT || exit 1
start_cluster

# ---------------------------------------------------------------- 점검
echo "[1] 관제 경로 (node1 = 127.0.0.1:18381)"
N1="http://127.0.0.1:18381"
code=$(curl -s -D "$TMP/h" -o "$TMP/stats.json" -w '%{http_code}' "$N1/__stats")
expect "GET /__stats 상태 코드" 200 "$code"
expect "  Content-Type" "application/json; charset=utf-8" "$(hdr "$TMP/h" Content-Type)"
expect "  Access-Control-Allow-Origin (다른 포트의 관제 화면이 읽을 수 있게)" "*" "$(hdr "$TMP/h" Access-Control-Allow-Origin)"
expect "  Cache-Control" "no-store" "$(hdr "$TMP/h" Cache-Control)"
expect "  Content-Length = 실제 본문 길이" "$(wc -c < "$TMP/stats.json" | tr -d ' ')" "$(hdr "$TMP/h" Content-Length)"
schema=$(python3 - "$TMP/stats.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
need = {"node", "mode", "uptime_ms", "active_connections", "requests", "outcomes", "origin_requests",
        "peer_failures", "bytes", "latency", "cache", "config", "cluster"}
errs = []
if not need <= set(d): errs.append(f"없는 키 {sorted(need - set(d))}")
if set(d["outcomes"]) != {"HIT", "VALIDATED", "STALE", "MISS", "REFRESH", "FORWARD", "BLOCKED", "ERROR"}: errs.append("outcomes 키")
b = d["latency"]["bounds_us"]
if len(b) != 26 or b != sorted(b): errs.append("bounds_us")
for k in ("client", "peer"):
    if len(d["latency"][k]["hist"]) != 27: errs.append(f"{k} hist 길이")
if d["node"] != "node1" or d["mode"] != "cluster": errs.append("node/mode")
if [(c["name"], c["port"], c["self"]) for c in d["cluster"]] != [("node1", 18381, True), ("node2", 18382, False), ("node3", 18383, False)]:
    errs.append("cluster 목록")
if d["active_connections"] < 1: errs.append("active_connections")
if d["config"]["validate_sec"] != 600 or d["config"]["peer_retry_ms"] != 3000: errs.append("config")
print("; ".join(errs) or "ok")
EOF
)
expect "  JSON 구조 (키, 히스토그램 27칸, 클러스터 목록, 설정값)" ok "$schema"
expect "GET /__stats?x=1 (쿼리는 무시)" 200 "$(curl -s -o /dev/null -w '%{http_code}' "$N1/__stats?x=1")"
code=$(curl -s -D "$TMP/h" -o "$TMP/dash.html" -w '%{http_code}' "$N1/__dashboard")
expect "GET /__dashboard 상태 코드" 200 "$code"
expect "  Content-Type" "text/html; charset=utf-8" "$(hdr "$TMP/h" Content-Type)"
if cmp -s "$TMP/dash.html" web/dashboard.html; then ok "  본문 = web/dashboard.html"; else ng "  본문이 web/dashboard.html과 다름"; fi
code=$(curl -s -D "$TMP/h" -o /dev/null -w '%{http_code}' "$N1/")
expect "GET / → 관제 화면으로 이동" "302 /__dashboard" "$code $(hdr "$TMP/h" Location)"
code=$(curl -s -D "$TMP/h" -o /dev/null -w '%{http_code}' "$N1/favicon.ico")
clen=$(hdr "$TMP/h" Content-Length)
expect "GET /favicon.ico (본문 없음, Content-Length 없음)" "204 -" "$code ${clen:--}"
expect "GET /__nope" 404 "$(curl -s -o /dev/null -w '%{http_code}' "$N1/__nope")"
expect "POST /__stats" 501 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$N1/__stats")"
c0=$(sget 18381 "d['requests']['client']")
for _ in 1 2 3 4 5; do curl -s -o /dev/null "$N1/__stats"; curl -s -o /dev/null "$N1/__dashboard"; done
expect "관제 요청은 통계에 세지 않음 (요청 10개 뒤 client 수)" "$c0" "$(sget 18381 "d['requests']['client']")"

# 관제 화면 파일 찾기: 다른 폴더에서 실행 → 실행 파일 기준으로 찾음 / 파일이 없는 곳의 복사본 → 404
(cd "$TMP" && exec "$ROOT/$BIN" -n far -p 18387 -q >> "$LOGDIR/test4_far.log" 2>&1) &
PID[far]=$!
mkdir -p "$TMP/x/a/b" && cp "$BIN" "$TMP/x/a/b/proxy"
(cd "$TMP" && exec "$TMP/x/a/b/proxy" -n nodash -p 18388 -q >> "$LOGDIR/test4_nodash.log" 2>&1) &
PID[nodash]=$!
wait_port 18387 && wait_port 18388
expect "다른 폴더에서 실행해도 관제 화면 제공 (실행 파일 기준)" 200 "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18387/__dashboard)"
expect "관제 화면 파일이 없으면 /__dashboard" 404 "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18388/__dashboard)"
expect "  그래도 /__stats는 제공" 200 "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18388/__stats)"
if grep -q "관제 화면 파일(web/dashboard.html)을 찾지 못했습니다" "$LOGDIR/test4_nodash.log"; then
    ok "  시작할 때 안내 문구 출력"
else
    ng "  시작 안내 문구 없음"
fi
stop_proc far
stop_proc nodash

echo "[2] 부하 생성기 요청열 (--dry-run, 요청 20000개)"
"${LG[@]}" --proxies "$CL" --dry-run -n 20000 --json "$TMP/dry.json" > "$TMP/dry.txt"
expect "실행 결과" 0 "$?"
for m in 1 10 20; do
    th=$(jget "$TMP/dry.json" "d['dry_run']['top_share']['$m']['theory']")
    sm=$(jget "$TMP/dry.json" "d['dry_run']['top_share']['$m']['sample']")
    truth "인기 상위 ${m}개의 요청 비율: 이론 $(printf '%.4f' "$th") ≈ 요청열 $(printf '%.4f' "$sm") (차이 0.02 미만)" \
          "abs($th - $sm) < 0.02"
done
"${LG[@]}" --proxies "$CL" --dry-run -n 3000 --json "$TMP/dry1.json" > /dev/null
"${LG[@]}" --proxies "$CL" --dry-run -n 3000 --json "$TMP/dry2.json" > /dev/null
"${LG[@]}" --proxies "$CL" --dry-run -n 3000 --seed 7 --json "$TMP/dry3.json" > /dev/null
r1=$(jget "$TMP/dry1.json" "d['dry_run']"); r2=$(jget "$TMP/dry2.json" "d['dry_run']"); r3=$(jget "$TMP/dry3.json" "d['dry_run']")
if [ -n "$r1" ] && [ "$r1" = "$r2" ]; then ok "같은 시드 → 같은 요청열"; else ng "같은 시드인데 요청열이 다름"; fi
if [ -n "$r3" ] && [ "$r1" != "$r3" ]; then ok "다른 시드 → 다른 요청열"; else ng "시드를 바꿔도 요청열이 같음"; fi

echo "[3] 클러스터 정확성: 빈 캐시에 요청 3000개를 하나씩 (동시 1)"
origin_reset
"${LG[@]}" --proxies "$CL" -n 3000 -c 1 --stats --json "$TMP/cl.json" --csv "$TMP/cl.csv" > "$TMP/cl.txt"
rc=$?
expect "부하 생성기 종료 코드 (0 = 실패 없음, --stats 확인 모두 일치)" 0 "$rc"
[ "$rc" -eq 0 ] || sed 's/^/      /' "$TMP/cl.txt"
expect "실패한 요청 (CRC32로 내용까지 비교)" 0 "$(jget "$TMP/cl.json" "d['result']['failed']")"
pred_cl=$(jget "$TMP/cl.json" "d['predict_origin']['cluster']")
pred_ind=$(jget "$TMP/cl.json" "d['predict_origin']['independent']")
origin_cl=$(origin_total)
expect "원서버 요청 수 = 예측값 (서로 다른 객체마다 정확히 한 번)" "$pred_cl" "$origin_cl"
objs=0; for p in 18381 18382 18383; do objs=$((objs + $(sget $p "d['cache']['objects']"))); done
expect "세 노드 캐시 객체 수의 합 = 서로 다른 객체 수 (단일 사본)" "$pred_cl" "$objs"
hist_ok=$(for p in 18381 18382 18383; do sget $p "all(sum(d['latency'][k]['hist']) == d['latency'][k]['count'] == d['requests'][k] for k in ('client', 'peer'))"; done | sort -u | tr '\n' ' ')
expect "노드마다 응답 시간 히스토그램 합 = 요청 수 (client, peer)" "True " "$hist_ok"
truth "히트율 $(jget "$TMP/cl.json" "round(100 * d['result']['hit_ratio'], 1)")% (90% 이상)" "$(jget "$TMP/cl.json" "d['result']['hit_ratio']") >= 0.9"
expect "CSV 기록 줄 수 (머리줄 + 요청 3000)" 3001 "$(wc -l < "$TMP/cl.csv" | tr -d ' ')"
grep -E '^\s+✓|^\s+✗' "$TMP/cl.txt" | sed 's/^ */      /'

echo "[4] 독립 프록시 비교: 같은 요청열을 독립 프록시 3개에 (동시 1)"
start_independent
origin_reset
"${LG[@]}" --proxies "$IND" -n 3000 -c 1 --stats --json "$TMP/ind.json" > "$TMP/ind.txt"
expect "부하 생성기 종료 코드" 0 "$?"
origin_ind=$(origin_total)
expect "원서버 요청 수 = 예측값 (진입 노드마다 객체마다 한 번)" "$pred_ind" "$origin_ind"
objs=0; for p in 18391 18392 18393; do objs=$((objs + $(sget $p "d['cache']['objects']"))); done
expect "세 프록시 캐시 객체 수의 합 = 원서버 요청 수 (같은 객체를 따로 저장)" "$origin_ind" "$objs"
expect "독립 프록시의 /__stats mode와 클러스터 목록 길이" "standalone 0" "$(sget 18391 "d['mode'] + ' ' + str(len(d['cluster']))")"
truth "원서버 요청: 독립 $origin_ind / 클러스터 $origin_cl = $(python3 -c "print(f'{$origin_ind / $origin_cl:.2f}')")배 (2배 이상)" \
      "$origin_ind >= 2 * $origin_cl"
for n in node1 node2 node3; do stop_proc "i$n"; done

echo "[5] 동시 부하에서 통계 불변식: 클러스터를 새로 띄우고 요청 4000개, 동시 32"
for n in node1 node2 node3; do stop_proc "$n"; done
start_cluster
origin_reset
"${LG[@]}" --proxies "$CL" -n 4000 -c 32 --seed 7 --stats --json "$TMP/con.json" > "$TMP/con.txt"
rc=$?
expect "부하 생성기 종료 코드 (실패 없음 + 노드 통계 확인 4개 일치)" 0 "$rc"
[ "$rc" -eq 0 ] || sed 's/^/      /' "$TMP/con.txt"
grep -E '^\s+✓|^\s+✗' "$TMP/con.txt" | sed 's/^ */      /'
pred=$(jget "$TMP/con.json" "d['predict_origin']['cluster']")
got=$(origin_total)
truth "원서버 요청 $got ≥ 동시 요청이 없을 때의 예측 $pred (같은 객체의 미스가 겹치면 더 많아진다)" "$got >= $pred"
hist_ok=$(for p in 18381 18382 18383; do sget $p "all(sum(d['latency'][k]['hist']) == d['latency'][k]['count'] == d['requests'][k] for k in ('client', 'peer'))"; done | sort -u | tr '\n' ' ')
expect "노드마다 히스토그램 합 = 요청 수 (동시 기록에서도 빠짐 없음)" "True " "$hist_ok"

echo "[6] 부하 중 /__stats 응답 시간 (동시 32로 6초 동안 부하)"
"${LG[@]}" --proxies "$CL" -d 6 -c 32 --seed 11 --json "$TMP/bg.json" > "$TMP/bg.txt" &
BG=$!
sleep 1
worst=0
codes=""
for _ in 1 2 3 4 5 6 7 8; do
    out=$(curl -s -o /dev/null -w '%{http_code} %{time_total}' --max-time 5 http://127.0.0.1:18382/__stats)
    codes="$codes${out%% *} "
    worst=$(python3 -c "print(max($worst, ${out##* }))")
    sleep 0.5
done
wait $BG
bg_rc=$?
expect "부하 중 /__stats 8번 모두 200" "200 200 200 200 200 200 200 200 " "$codes"
truth "  가장 느린 응답 ${worst}초 (1초 미만)" "$worst < 1.0"
expect "  같은 시간 부하 생성기 종료 코드" 0 "$bg_rc"
truth "  부하 생성기가 보낸 요청 $(jget "$TMP/bg.json" "d['result']['requests']")개 (100개 이상)" \
      "$(jget "$TMP/bg.json" "d['result']['requests']") >= 100"

echo "[7] 진입 노드 장애: 목록 맨 앞에 아무도 듣지 않는 포트 $DEAD (요청 800개 중 200개가 그 노드 차례)"
"${LG[@]}" --proxies "127.0.0.1:$DEAD,$CL" -n 800 -c 8 --stats --json "$TMP/dead.json" > "$TMP/dead.txt"
expect "다음 노드로 다시 연결: 종료 코드" 0 "$?"
expect "  실패한 요청" 0 "$(jget "$TMP/dead.json" "d['result']['failed']")"
expect "  다시 연결한 횟수 (죽은 노드 차례인 요청 수)" 200 "$(jget "$TMP/dead.json" "d['result']['retries']")"
"${LG[@]}" --proxies "127.0.0.1:$DEAD,$CL" -n 800 -c 8 --no-retry --json "$TMP/noretry.json" > /dev/null
expect "--no-retry: 종료 코드 (1 = 실패 있음)" 1 "$?"
expect "  실패한 요청 수 (죽은 노드 차례인 요청)" 200 "$(jget "$TMP/noretry.json" "d['result']['failed']")"
expect "  실패 원인" "연결 실패 200" "$(jget "$TMP/noretry.json" "' '.join(f'{k} {v}' for k, v in d['result']['errors'].items())")"

echo "[8] 정상 종료, 통계, 새니타이저"
for n in node1 node2 node3; do stop_proc "$n"; done
if [[ "$RCS" =~ ^0+$ ]]; then
    ok "프록시 ${#RCS}개 모두 종료 코드 0"
else
    ng "종료 코드: $RCS (모두 0이어야 함)"
fi
if grep -q "관제 화면: http://127.0.0.1:18381/__dashboard" "$LOGDIR/test4_node1.log"; then
    ok "시작할 때 관제 화면 주소 출력"
else
    ng "시작 로그에 관제 화면 주소 없음"
fi
if grep -q "통계: 히트율" "$LOGDIR/test4_node1.log"; then ok "종료 시 통계 출력"; else ng "종료 통계 없음"; fi
reports=$(cat "$LOGDIR"/test4_*.log | grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:')
expect "새니타이저 경고 수 (MODE=$MODE)" 0 "$reports"

echo
echo "결과: 통과 $pass개, 실패 $fail개   (로그: $LOGDIR/test4_*.log)"
[ "$fail" -eq 0 ]

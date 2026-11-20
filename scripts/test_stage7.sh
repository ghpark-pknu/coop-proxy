#!/usr/bin/env bash
# 7단계(실험 분석 도구) 점검: 측정 보강, 요약 표, 그래프, 실험 ② 스크립트가 끝까지 도는가
#
# 실험 스크립트를 아주 작은 크기로 실제로 돌려서 결과 파일의 모양과 도구들을 확인한다.
# 숫자의 크기(성능)는 확인하지 않는다: 그건 실험이 할 일이다.
# 시험용 포트: 18781(프록시), 19781(원서버), 실험 스크립트가 쓰는 18600·19600, 18701~18703·19700
set -u
cd "$(dirname "$0")/.." || exit 1

MODE=${MODE:-debug}
DATA=${COOP_DATA:-$HOME/coop-data}
LOGDIR="$DATA/logs"
export COOP_DATA="$DATA"

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

pass=0
fail=0
ok() { printf '  \033[32m✓\033[0m %s\n' "$1"; pass=$((pass + 1)); }
ng() { printf '  \033[31m✗\033[0m %s\n' "$1"; fail=$((fail + 1)); }
expect() {
    if [ "$3" = "$2" ]; then ok "$1 → $3"; else ng "$1 → ${3:-없음} (기대값 $2)"; fi
}
truth() {
    if python3 -c "import sys; sys.exit(0 if ($2) else 1)" 2>/dev/null; then ok "$1"; else ng "$1"; fi
}
jget() {
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print($2)" "$1" 2>/dev/null
}
newest() { ls -t $1 2>/dev/null | head -1; }

echo "[준비] 빌드 (MODE=$MODE)"
make -s MODE="$MODE" all loadc ctest && make -s MODE=release all loadc ctest || { echo "빌드 실패"; exit 1; }
mkdir -p "$LOGDIR" results
rm -f "$LOGDIR"/test7_*.log
TMP=$(mktemp -d)
PIDS=""
cleanup() {
    for p in $PIDS; do kill -TERM "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT
wait_port() {
    for _ in $(seq 1 100); do nc -z 127.0.0.1 "$1" 2>/dev/null && return 0; sleep 0.1; done
    echo "  포트 $1 이 열리지 않았습니다"; return 1
}
[ -f "$DATA/www-small/manifest.json" ] || python3 tools/gen_files.py --dir "$DATA/www-small" --min 1024 --max 1024 > /dev/null

# ---------------------------------------------------------------- 점검
echo "[1] loadc: 메인(accept) 스레드 CPU를 따로 잰다 (MODE=$MODE)"
python3 tools/origin.py --port 19781 --dir "$DATA/www-small" --quiet > "$LOGDIR/test7_origin.log" 2>&1 &
PIDS="$PIDS $!"
"build/$MODE/proxy" -p 18781 -q -v 600 > "$LOGDIR/test7_proxy.log" 2>&1 &
PP=$!
PIDS="$PIDS $PP"
wait_port 19781 && wait_port 18781 || exit 1
"build/$MODE/loadc" -x 127.0.0.1:18781 -o 127.0.0.1:19781 -c 4 -d 1 --prefill --pid "$PP" -q \
    --json "$TMP/m.json" > "$TMP/m.txt" 2>> "$LOGDIR/test7_loadc.log"
expect "종료 코드" 0 "$?"
truth "메인 스레드 $(jget "$TMP/m.json" "d['result']['cpu']['proxy_main_cores']")코어 ≤ 프록시 전체 $(jget "$TMP/m.json" "d['result']['cpu']['proxy_cores']")코어, 둘 다 0보다 큼" \
      "$(jget "$TMP/m.json" "0 < d['result']['cpu']['proxy_main_cores'] <= d['result']['cpu']['proxy_cores'] + 0.02")"
truth "요청당 메인 스레드 CPU ≤ 요청당 프록시 CPU" \
      "$(jget "$TMP/m.json" "d['result']['cpu']['proxy_main_us_per_req'] <= d['result']['cpu']['proxy_us_per_req'] + 0.5")"
if grep -q "그중 메인(accept) 스레드" "$TMP/m.txt"; then ok "화면 출력에 메인 스레드 표시"; else ng "화면 출력에 메인 스레드 없음"; fi
kill -TERM "$PP"; wait "$PP"
expect "프록시 종료 코드" 0 "$?"

echo "[2] 해시: Python(exp_common.py) = C(include/hash.h)"
expect "FNV-1a, mix64 값 (C로 미리 계산한 값과 비교)" "0xa2aa7d3301e7a7e7 0xb65eb8cb9387ee89" \
       "$(cd tools && python3 -c "from exp_common import *; print(hex(fnv1a('http://127.0.0.1:9000/obj/0000.bin')), hex(mix64(fnv1a('node1'))))")"
expect "Σp² 예측 (샤드 2·4·8·16, 6단계 분석에서 C로 구한 값)" "0.523 0.284 0.152 0.083" \
       "$(cd tools && python3 -c "from exp_common import *; print(' '.join(f'{sum_sq(\"127.0.0.1:19600\", 200, 0.8, s):.3f}' for s in (2, 4, 8, 16)))")"

echo "[3] 락 실험 (샤드 1·4 × 동시 2·8, 반복 2번, 1초씩)"
SHARDS="1 4" CONCS="2 8" REPEAT=2 DUR=1 WARM=1 ./scripts/exp_lock.sh > "$TMP/lock.txt" 2>&1
expect "exp_lock.sh 종료 코드" 0 "$?"
CSV=$(newest "results/lock_*[0-9].csv")
expect "CSV 줄 수 (머리줄 + 측정 8회)" 9 "$(wc -l < "$CSV" | tr -d ' ')"
expect "CSV 열 수 (6단계 19열 + 7단계 3열)" 22 "$(head -1 "$CSV" | tr ',' '\n' | wc -l | tr -d ' ')"
expect "반복 2는 샤드 순서를 거꾸로 (반복 2의 첫 측정의 샤드 수)" 4 "$(awk -F, '$1 == 2 { print $2; exit }' "$CSV")"
truth "메인 스레드 CPU 열이 모두 채워짐" "all(r['main_cores'] != '' for r in __import__('csv').DictReader(open('$CSV')))"
if grep -q "^origin=127.0.0.1:19600" "${CSV%.csv}.env"; then ok ".env에 원서버 주소 기록"; else ng ".env에 origin 없음"; fi
expect "같은 CPU의 마이크로 벤치마크 CSV (머리줄 + 샤드 2개)" 3 "$(wc -l < "${CSV%.csv}_bench.csv" | tr -d ' ')"
python3 tools/lock_summary.py "$CSV" > "$TMP/sum.txt" 2>&1
expect "lock_summary.py 종료 코드" 0 "$?"
for t in "⑧ 캐시 락 경합률" "⑨ 요청 하나가" "⑩ 메인(accept)" "⑪ 같은 CPU에서"; do
    if grep -q "$t" "$TMP/sum.txt"; then ok "요약에 '$t' 표"; else ng "요약에 '$t' 표 없음"; fi
done
python3 tools/plot_results.py lock "$CSV" > "$TMP/plot.txt" 2>&1
expect "plot_results.py lock 종료 코드" 0 "$?"
n=$(find "${CSV%.csv}_figs" -name '*.png' -size +10k | wc -l)
truth "그림 파일 ${n}개 (처리량, 배율 비교, 요청당 CPU — 경합률 그림은 경합이 적으면 생략)" "$n >= 3"

echo "[4] 6단계 형식(19열) CSV도 읽는가"
cut -d, -f1-19 "$CSV" > "$TMP/old.csv"
python3 tools/lock_summary.py "$TMP/old.csv" > "$TMP/old.txt" 2>&1
expect "lock_summary.py 종료 코드" 0 "$?"
if grep -q "이 CSV에는 없음" "$TMP/old.txt"; then ok "없는 열의 표는 건너뛴다고 안내"; else ng "옛 형식 안내 없음"; fi
python3 tools/plot_results.py lock "$TMP/old.csv" > /dev/null 2>&1
expect "plot_results.py lock 종료 코드" 0 "$?"
truth "그림 파일 2개 이상" "$(find "$TMP/old_figs" -name '*.png' -size +10k | wc -l) >= 2"

echo "[5] 실험 ②: 독립 대 클러스터 (노드당 1M, 요청 600개, 예열 200개)"
SIZES="1M" N=600 WARMUP=200 REPEAT=1 ./scripts/exp_cluster.sh > "$TMP/cl.txt" 2>&1
expect "exp_cluster.sh 종료 코드" 0 "$?"
CCSV=$(newest "results/cluster_*[0-9].csv")
expect "CSV 줄 수 (머리줄 + 측정 2회)" 3 "$(wc -l < "$CCSV" | tr -d ' ')"
read -r hi oi hc oc < <(python3 - "$CCSV" <<'EOF'
import csv, sys
r = {x["mode"]: x for x in csv.DictReader(open(sys.argv[1]))}
print(r["independent"]["hit_ratio"], r["independent"]["origin_requests"], r["cluster"]["hit_ratio"], r["cluster"]["origin_requests"])
EOF
)
truth "히트율: 클러스터 $hc ≥ 독립 $hi" "$hc >= $hi"
truth "원서버 요청: 클러스터 $oc < 독립 $oi" "$oc < $oi"
expect "실패한 요청" 0 "$(python3 -c "import csv; print(sum(int(r['failed']) for r in csv.DictReader(open('$CCSV'))))")"
truth "HIT 요청만의 평균 응답 시간이 채워짐" "all(r['hit_mean_ms'] != '' for r in __import__('csv').DictReader(open('$CCSV')))"
python3 tools/plot_results.py cluster "$CCSV" > /dev/null 2>&1
expect "plot_results.py cluster 종료 코드" 0 "$?"
expect "그림 파일 수" 2 "$(find "${CCSV%.csv}_figs" -name '*.png' -size +10k | wc -l | tr -d ' ')"

echo "[6] 새니타이저"
reports=$(cat "$LOGDIR"/test7_*.log 2>/dev/null | grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:')
expect "새니타이저 경고 수 (MODE=$MODE)" 0 "$reports"

echo
echo "결과: 통과 $pass개, 실패 $fail개   (점검이 만든 결과 파일: $CSV, $CCSV)"
[ "$fail" -eq 0 ]

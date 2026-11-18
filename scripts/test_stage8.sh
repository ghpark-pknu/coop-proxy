#!/usr/bin/env bash
# 8단계(검증·진단) 점검: 새 스크립트 네 개가 끝까지 돌고, 결과 파일과 판정이 제대로 나오는가
#
# 사용법:  ./scripts/test_stage8.sh      (약 2분. valgrind가 필요: sudo apt install valgrind)
#
# 스크립트를 아주 짧게 실제로 돌려서 결과의 모양을 확인한다.
# 성능 수치와 장시간 판정(메모리 기울기)은 확인하지 않는다: 그건 본 실행이 할 일이다.
# 쓰는 포트: 18620·19620(diag_main), 18801~18803·19800(soak), 18811·18812·19810(memcheck),
#            18701~18703·19700(exp_cluster)
set -u
cd "$(dirname "$0")/.." || exit 1

DATA=${COOP_DATA:-$HOME/coop-data}
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
has() {   # has 설명 파일 찾을글자
    if grep -qF -- "$3" "$2"; then ok "$1"; else ng "$1 (출력에 '$3' 없음)"; fi
}
newest() { ls -t $1 2>/dev/null | head -1; }
cget() {  # cget CSV 열이름 [조건식]: 조건을 만족하는 첫 줄의 값
    python3 -c "
import csv, sys
for r in csv.DictReader(open(sys.argv[1])):
    if eval(sys.argv[3]):
        print(r[sys.argv[2]]); break" "$1" "$2" "${3:-True}" 2>/dev/null
}
mkdir -p results
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "[1] diag_main.sh: 메인 스레드 진단 (동시 16, 측정 5초)"
CONCS="16" DUR=5 WARM=1 ./scripts/diag_main.sh > "$TMP/diag.txt" 2>&1
expect "종료 코드" 0 "$?"
for t in "① accept 대기열" "② 메인 스레드의 시간" "③ 메인 스레드의 문맥 교환" "판정:"; do
    has "출력에 '$t'" "$TMP/diag.txt" "$t"
done
D=$(newest "results/diag_*.csv")
expect "CSV 줄 수 (머리줄 + 동시 연결 수 1개)" 2 "$(wc -l < "$D" | tr -d ' ')"
truth "accept 대기열 표본 $(cget "$D" samples)개 ≥ 20 (0.1초마다 3초 동안)" "$(cget "$D" samples) >= 20"
if [ -n "$(cget "$D" main_runq_pct)" ]; then
    truth "메인 스레드: 실행 $(cget "$D" main_on_pct)% + 실행 대기 $(cget "$D" main_runq_pct)% + 잠듦 $(cget "$D" main_sleep_pct)% = 100%" \
          "abs($(cget "$D" main_on_pct) + $(cget "$D" main_runq_pct) + $(cget "$D" main_sleep_pct) - 100) < 0.5"
else
    ok "schedstat이 없는 커널: 실행 대기·잠듦은 건너뜀"
fi
truth "schedstat의 메인 스레드 실행 시간 $(cget "$D" main_on_us)us ≈ loadc가 잰 $(cget "$D" loadc_main_us)us (0.5~2배)" \
      "0.5 <= $(cget "$D" main_on_us) / $(cget "$D" loadc_main_us) <= 2"
DUR=2 ./scripts/diag_main.sh > "$TMP/bad.txt" 2>&1
expect "DUR=2는 거부 (종료 코드)" 1 "$?"

echo "[2] soak.sh: 장시간 부하 (20초, 2초마다 기록)"
SEC=20 INTERVAL=2 ./scripts/soak.sh > "$TMP/soak.txt" 2>&1
expect "종료 코드" 0 "$?"
S=$(newest "results/soak_*[0-9].csv")
expect "노드 3개 × 부하 전 기록" 3 "$(grep -c ',before,' "$S")"
expect "노드 3개 × 부하 뒤 기록" 3 "$(grep -c ',after,' "$S")"
truth "부하 중 기록 $(grep -c ',load,' "$S")줄 ≥ 24 (노드 3개 × 8번 이상)" "$(grep -c ',load,' "$S") >= 24"
expect "열린 파일·스레드가 부하 전과 같음 (노드 3개 × 2)" 6 "$(grep -o '(같음 ✓)' "$TMP/soak.txt" | wc -l | tr -d ' ')"
expect "정상 종료한 노드" 3 "$(grep -c '정상 종료 ✓' "$TMP/soak.txt")"
has "실패한 요청 0개" "$TMP/soak.txt" "실패 0개"
has "짧은 실행은 메모리 판정을 참고용으로" "$TMP/soak.txt" "참고용"
truth "부하 뒤 /__stats의 진행 중 연결 0 (노드 3개)" \
      "all(int(r['active']) == 0 for r in __import__('csv').DictReader(open('$S')) if r['phase'] == 'after')"
python3 tools/plot_results.py soak "$S" > /dev/null 2>&1
expect "plot_results.py soak 종료 코드" 0 "$?"
truth "그림 파일 (10KB 이상)" "$(find "${S%.csv}_figs" -name 'soak_memory_fd.png' -size +10k | wc -l) == 1"

echo "[3] memcheck.sh: Valgrind (요청 600개)"
if command -v valgrind > /dev/null 2>&1; then
    N=600 ./scripts/memcheck.sh > "$TMP/mem.txt" 2>&1
    expect "종료 코드" 0 "$?"
    has "판정: 통과" "$TMP/mem.txt" "판정: 통과"
    V=$(newest "results/memcheck_*.log")
    has "Valgrind 보고서에 'ERROR SUMMARY: 0 errors'" "$V" "ERROR SUMMARY: 0 errors"
    for t in "차단 목록 blocked.test → 403" "없는 원서버 :9 +→ 502" "POST +→ 501" "잘못된 요청 줄 +→ HTTP/1.0 400"; do
        if grep -qE -- "$t" "$TMP/mem.txt"; then ok "특수 요청: $(sed 's/ +/ /' <<< "$t")"; else ng "특수 요청 응답이 다름: $t"; fi
    done
    expect "loadc 두 번 모두 성공" 2 "$(grep -c 'loadc 종료 코드 0' "$TMP/mem.txt")"
else
    ng "valgrind가 없습니다: sudo apt install valgrind"
fi

echo "[4] exp_cluster.sh RATE: 요청 속도 상한 (1M, 요청 300개, 초당 200개)"
SIZES="1M" N=300 WARMUP=100 REPEAT=1 RATE=200 ./scripts/exp_cluster.sh > "$TMP/cl.txt" 2>&1
expect "종료 코드" 0 "$?"
C=$(newest "results/cluster_*[0-9].csv")
if grep -q "rate=200" "${C%.csv}.env"; then ok ".env에 rate=200 기록"; else ng ".env에 rate 없음"; fi
truth "두 방식 모두 처리량 ≤ 220/s (독립 $(cget "$C" rps "r['mode']=='independent'"), 클러스터 $(cget "$C" rps "r['mode']=='cluster'"))" \
      "all(float(r['rps']) <= 220 for r in __import__('csv').DictReader(open('$C')))"
has "요약 표에 속도 상한 표시" "$TMP/cl.txt" "요청 속도 상한 초당 200개"
RATE=abc ./scripts/exp_cluster.sh > "$TMP/bad.txt" 2>&1
expect "RATE=abc는 거부 (종료 코드)" 1 "$?"

echo
echo "결과: 통과 $pass개, 실패 $fail개   (점검이 만든 결과 파일: $D, $S, ${V:-}, $C)"
[ "$fail" -eq 0 ]

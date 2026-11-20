#!/usr/bin/env bash
# 6단계 실험 ①: 캐시 락 전략별 처리량 — 전역 mutex(-S 1) 대 샤딩(-S N), 네트워크 포함
#
# 바꿀 수 있는 값
#   SHARDS="1 2 4 8 16"   CONCS="1 2 4 8 16 32 64"   REPEAT=3   DUR=10   WARM=2
#   ZIPF=0.8   KEYS=200
#   PROXY_CPUS, LOAD_CPUS  taskset 형식("0-3", "0,1,4,5"). 주지 않으면 물리 코어를 반으로 나눠
#                          앞쪽은 프록시, 뒤쪽은 부하 도구와 원서버에 준다 (하이퍼스레드 두 개는 같은 쪽)
#
# 측정 조건 — "요청 처리 + 캐시 락"만 남기기 위해
#   release 빌드, 1KB 객체 200개(~/coop-data/www-small), 캐시 64MB(교체 없음), 검증 주기 600초,
#   부하 도구가 측정 전에 모든 객체를 한 번씩 요청 → 측정 구간은 모두 HIT (원서버·디스크가 끼지 않음)
#   반복은 바깥 고리: 반복 1회 안에서 모든 샤드 수를 돌고 다음 반복으로 → 노트북의 온도·터보 변화가
#   특정 샤드 수에만 몰리지 않게 한다. 짝수 번째 반복은 샤드 순서를 거꾸로 돈다 (1→16, 16→1, ...):
#   "늘 먼저 재는 샤드 수"가 생기지 않게. 샤드 수마다 프록시를 새로 띄운다.
#   시작 전에 같은 프록시 CPU에서 캐시 마이크로 벤치마크(cache_test bench)를 같은 URL로 돌려
#   "자료구조만 쟀을 때"와 "프록시 전체"를 나란히 비교할 수 있게 한다 (SKIP_BENCH=1이면 생략).
#
# 결과: results/lock_날짜_시각.csv (측정 1회 = 한 줄), results/lock_날짜_시각.env (실험 환경),
#       results/lock_날짜_시각_bench.csv (마이크로 벤치마크)
#       마지막에 반복의 중앙값으로 요약 표를 출력한다. 표만 다시 보려면:
#       python3 tools/lock_summary.py results/lock_날짜_시각.csv
set -u
cd "$(dirname "$0")/.." || exit 1

if [ "${QUICK:-0}" = 1 ]; then
    SHARDS=${SHARDS:-"1 8"}; CONCS=${CONCS:-"8 32"}; REPEAT=${REPEAT:-1}; DUR=${DUR:-3}; WARM=${WARM:-1}
else
    SHARDS=${SHARDS:-"1 2 4 8 16"}; CONCS=${CONCS:-"1 2 4 8 16 32 64"}; REPEAT=${REPEAT:-3}; DUR=${DUR:-10}; WARM=${WARM:-2}
fi
ZIPF=${ZIPF:-0.8}
KEYS=${KEYS:-200}
DATA=${COOP_DATA:-$HOME/coop-data}
SMALL="$DATA/www-small"
LOGDIR="$DATA/logs"
PROXY_PORT=18600
ORIGIN_PORT=19600

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

# ---------------------------------------------------------------- CPU 나누기
# thread_siblings_list: 같은 물리 코어를 쓰는 논리 CPU 목록 (예: "0-1" 또는 "0,4")
if [ -z "${PROXY_CPUS:-}" ] || [ -z "${LOAD_CPUS:-}" ]; then
    mapfile -t CORE_SETS < <(for c in $(ls -d /sys/devices/system/cpu/cpu[0-9]* | sort -V); do
                                 cat "$c/topology/thread_siblings_list" 2>/dev/null
                             done | awk 'NF && !seen[$0]++')
    n=${#CORE_SETS[@]}
    if [ "$n" -ge 2 ]; then
        half=$(( (n + 1) / 2 ))
        PROXY_CPUS=$(IFS=,; echo "${CORE_SETS[*]:0:$half}")
        LOAD_CPUS=$(IFS=,; echo "${CORE_SETS[*]:$half}")
    else
        PROXY_CPUS=""; LOAD_CPUS=""
        echo "※ 물리 코어가 1개라서 CPU를 나누지 않습니다 (결과 해석에 주의)"
    fi
fi
# CPU를 고정해 실행한다 (목록이 비어 있으면 그냥 실행).
#   pin    : 앞에서 실행하고 끝나길 기다린다
#   pin_bg : 뒤에서 실행하고 그 프로세스 번호를 BG_PID에 넣는다
# pin_bg를 함수로 감싸 '&'로 부르면 안 된다: 그러면 bash가 함수를 돌릴 서브셸을 따로 만들어
# $!가 프록시가 아니라 그 서브셸의 번호가 된다 (CPU 측정·종료가 엉뚱한 프로세스에 간다).
# taskset은 CPU를 정한 뒤 exec로 명령으로 바뀌므로, 'taskset ... &'의 $!는 곧 명령의 번호다.
pin() {
    local cpus=$1
    shift
    if [ -n "$cpus" ]; then taskset -c "$cpus" "$@"; else "$@"; fi
}
BG_PID=""
pin_bg() {  # pin_bg CPU목록 기록파일 명령...
    local cpus=$1 log=$2
    shift 2
    if [ -n "$cpus" ]; then taskset -c "$cpus" "$@" > "$log" 2>&1 &
    else "$@" > "$log" 2>&1 &
    fi
    BG_PID=$!
}
ncpus_of() { python3 -c "
import sys
n = 0
for part in sys.argv[1].split(','):
    if part:
        a, _, b = part.partition('-'); n += int(b or a) - int(a) + 1
print(n)" "$1"; }

# ---------------------------------------------------------------- 준비
echo "[준비] release 빌드"
make -s MODE=release all loadc ctest || { echo "빌드 실패"; exit 1; }
BIN=build/release/proxy
LC=build/release/loadc
if [ ! -f "$SMALL/manifest.json" ]; then
    echo "[준비] 1KB 객체 200개 생성 → $SMALL"
    python3 tools/gen_files.py --dir "$SMALL" --min 1024 --max 1024 > /dev/null || exit 1
fi
mkdir -p results "$LOGDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
CSV="results/lock_$STAMP.csv"
ENVF="results/lock_$STAMP.env"
BENCH="results/lock_${STAMP}_bench.csv"
TMP=$(mktemp -d)
ORIGIN_PID=""
PP=""
cleanup() {
    [ -n "$PP" ] && kill -TERM "$PP" 2>/dev/null
    [ -n "$ORIGIN_PID" ] && kill -TERM "$ORIGIN_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT
wait_port() {
    for _ in $(seq 1 100); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "포트 $1 이 열리지 않았습니다 ($LOGDIR/exp_lock_*.log 확인)"
    exit 1
}
lockstat() {    # 프록시의 락 누적값: 캐시 획득 경합 대기us 통계획득 통계경합 통계대기us
    curl -s "http://127.0.0.1:$PROXY_PORT/__stats" | python3 -c "
import json, sys
d = json.load(sys.stdin)['locks']
print(d['cache']['acquires'], d['cache']['contended'], d['cache']['wait_us'],
      d['stats']['acquires'], d['stats']['contended'], d['stats']['wait_us'])"
}

NP=$(ncpus_of "$PROXY_CPUS"); NL=$(ncpus_of "$LOAD_CPUS")
runs=$(( REPEAT * $(wc -w <<< "$SHARDS") * $(wc -w <<< "$CONCS") ))
{
    echo "date=$(date '+%Y-%m-%d %H:%M:%S')"
    echo "commit=$(git rev-parse --short HEAD 2>/dev/null)$(git diff --quiet 2>/dev/null || echo '+수정됨')"
    echo "cpu_model=$(lscpu | sed -n 's/^Model name: *//p')"
    echo "cpus=$(nproc) threads_per_core=$(lscpu | sed -n 's/^Thread(s) per core: *//p')"
    echo "kernel=$(uname -r)"
    echo "proxy_cpus=${PROXY_CPUS:-전체} load_cpus=${LOAD_CPUS:-전체}"
    echo "shards=$SHARDS concs=$CONCS repeat=$REPEAT dur=$DUR warm=$WARM zipf=$ZIPF keys=$KEYS"
    echo "proxy_args=-q -v 600 -s 64M -S <샤드 수>   objects=$SMALL (1KB × $KEYS)"
    echo "origin=127.0.0.1:$ORIGIN_PORT"
} > "$ENVF"

# 앞 19열은 6단계 형식 그대로 두고 7단계 열을 뒤에 붙인다 (옛 결과 파일도 같은 요약·그래프 도구로 읽히게)
echo "rep,shards,conc,rps,mean_ms,p50_ms,p90_ms,p99_ms,failed,loader_cores,proxy_cores,proxy_us_per_req,cache_acquires,cache_contended,cache_contended_pct,cache_wait_us,stats_acquires,stats_contended,stats_contended_pct,main_cores,main_us_per_req,stats_wait_us" > "$CSV"

echo "[준비] 프록시 CPU ${PROXY_CPUS:-전체} (${NP}개) / 부하 도구·원서버 CPU ${LOAD_CPUS:-전체} (${NL}개)"
echo "[준비] 측정 ${runs}회 × (예열 ${WARM}초 + 측정 ${DUR}초) ≈ $(python3 -c "print(int($runs * ($WARM + $DUR + 1) / 60) + 1)")분"
for p in $ORIGIN_PORT $PROXY_PORT; do
    if nc -z 127.0.0.1 "$p" 2>/dev/null; then
        echo "포트 $p 를 이미 다른 프로그램이 쓰고 있습니다 (ss -ltnp | grep $p 로 확인 후 종료하세요)"
        exit 1
    fi
done
if [ "${SKIP_BENCH:-0}" != 1 ]; then
    echo "[준비] 같은 프록시 CPU(${NP}개)에서 캐시 마이크로 벤치마크: 스레드 ${NP}개, 샤드 $SHARDS"
    pin "$PROXY_CPUS" build/release/cache_test bench -t "$NP" -d 2 -k "$KEYS" -z "$ZIPF" \
        -S "$(tr ' ' ',' <<< "$SHARDS")" --prefix "http://127.0.0.1:$ORIGIN_PORT/obj/" --csv "$BENCH" \
        | sed -n '/샤드  처리량/,/^$/p' | sed 's/^/  /'
fi
pin_bg "$LOAD_CPUS" "$LOGDIR/exp_lock_origin.log" python3 tools/origin.py --port $ORIGIN_PORT --dir "$SMALL" --quiet
ORIGIN_PID=$BG_PID
wait_port $ORIGIN_PORT

# ---------------------------------------------------------------- 측정
i=0
REVERSED=$(tr ' ' '\n' <<< "$SHARDS" | grep -v '^$' | tac | tr '\n' ' ')
for rep in $(seq 1 "$REPEAT"); do
    order=$SHARDS
    [ $((rep % 2)) -eq 0 ] && order=$REVERSED       # 짝수 번째 반복은 거꾸로
    for s in $order; do
        if nc -z 127.0.0.1 $PROXY_PORT 2>/dev/null; then
            echo "포트 $PROXY_PORT 가 아직 열려 있습니다 (앞 프록시가 끝나지 않음)"; exit 1
        fi
        pin_bg "$PROXY_CPUS" "$LOGDIR/exp_lock_proxy_S$s.log" "$BIN" -n exp -p $PROXY_PORT -q -v 600 -s 64M -S "$s"
        PP=$BG_PID
        wait_port $PROXY_PORT
        # 지금 포트를 연 것이 방금 띄운 프록시인가 (샤드 수까지 확인)
        if [ "$(cat /proc/$PP/comm 2>/dev/null)" != "proxy" ] || \
           [ "$(curl -s "http://127.0.0.1:$PROXY_PORT/__stats" | python3 -c "import json,sys; print(json.load(sys.stdin)['cache']['shards'])")" != "$s" ]; then
            echo "프록시(pid $PP, 샤드 $s)가 제대로 뜨지 않았습니다 → $LOGDIR/exp_lock_proxy_S$s.log"; exit 1
        fi
        for c in $CONCS; do
            i=$((i + 1))
            read -r a0 c0 w0 sa0 sc0 sw0 < <(lockstat)
            pin "$LOAD_CPUS" "$LC" -x 127.0.0.1:$PROXY_PORT -o 127.0.0.1:$ORIGIN_PORT -k "$KEYS" -z "$ZIPF" \
                -c "$c" -d "$DUR" -w "$WARM" --prefill --pid "$PP" -q --json "$TMP/r.json" > "$TMP/r.txt" 2>&1
            rc=$?
            read -r a1 c1 w1 sa1 sc1 sw1 < <(lockstat)
            if [ $rc -eq 2 ] || [ ! -s "$TMP/r.json" ]; then
                echo "  측정 실패 (샤드 $s, 동시 $c):"; sed 's/^/    /' "$TMP/r.txt"; exit 1
            fi
            row=$(python3 - "$TMP/r.json" "$rep" "$s" "$c" "$a0" "$c0" "$w0" "$sa0" "$sc0" "$sw0" "$a1" "$c1" "$w1" "$sa1" "$sc1" "$sw1" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))["result"]
rep, s, c = sys.argv[2:5]
a0, c0, w0, sa0, sc0, sw0, a1, c1, w1, sa1, sc1, sw1 = map(int, sys.argv[5:17])
ca, cc, sa, sc = a1 - a0, c1 - c0, sa1 - sa0, sc1 - sc0
L, cpu = d["latency_ms"], d["cpu"]
f = lambda v: "" if v is None else f"{v:.4f}"
print(",".join([rep, s, c, f"{d['rps']:.1f}", f(L["mean"]), f(L["p50"]), f(L["p90"]), f(L["p99"]),
                str(d["failed"]), f(cpu["loader_cores"]), f(cpu["proxy_cores"]), f(cpu["proxy_us_per_req"]),
                str(ca), str(cc), f"{100 * cc / ca:.3f}" if ca else "0", str(w1 - w0),
                str(sa), str(sc), f"{100 * sc / sa:.3f}" if sa else "0",
                f(cpu.get("proxy_main_cores")), f(cpu.get("proxy_main_us_per_req")), str(sw1 - sw0)]))
EOF
)
            echo "$row" >> "$CSV"
            IFS=, read -r _ _ _ rps _ _ _ p99 failed lcores pcores pus _ _ cpct _ _ _ spct mcores _ _ <<< "$row"
            printf '  [%3d/%d] 반복 %d  샤드 %2d  동시 %2d → %9.0f 요청/s  p99 %6.3fms  프록시 %.2f코어(요청당 %.1fus, 메인 스레드 %.2f)  부하 도구 %.2f코어  경합 캐시 %s%% 통계 %s%%%s\n' \
                   "$i" "$runs" "$rep" "$s" "$c" "$rps" "$p99" "$pcores" "$pus" "${mcores:-0}" "$lcores" "$cpct" "$spct" \
                   "$([ "$failed" != 0 ] && echo "  ※ 실패 $failed")"
        done
        kill -TERM "$PP"
        wait "$PP"
        PP=""
        for _ in $(seq 1 50); do nc -z 127.0.0.1 $PROXY_PORT 2>/dev/null || break; sleep 0.1; done
    done
done

echo
echo "결과 파일: $CSV"
echo "실험 환경: $ENVF"
[ -s "$BENCH" ] && echo "마이크로 벤치마크: $BENCH"
python3 tools/lock_summary.py "$CSV" --proxy-cpus "$NP" --load-cpus "$NL"
echo
echo "그래프: python3 tools/plot_results.py lock $CSV"

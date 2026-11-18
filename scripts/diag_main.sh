#!/usr/bin/env bash
# 8단계 진단: 실험 ①의 처리량 천장(약 16k 요청/s)이 메인(accept) 스레드 때문인가
#
# 사용법
#   ./scripts/diag_main.sh                    동시 8·64, 각 15초 (약 1분)
#   CONCS="64" DUR=30 ./scripts/diag_main.sh
# 바꿀 수 있는 값: CONCS="8 64"  DUR=15  WARM=2  SHARDS=1  KEYS=200  ZIPF=0.8
#                  PROXY_CPUS, LOAD_CPUS (taskset 형식. 주지 않으면 exp_lock.sh처럼 물리 코어를 반으로 나눔)
#
# 실험 ①과 같은 조건(release, 1KB 객체 200개, 측정 구간은 모두 HIT, 프록시와 부하 도구의 CPU를 나눔)에서
# 측정 구간 동안 세 가지를 잰다.
#   ① accept 대기열: 연결 수립(3-way handshake)은 끝났지만 프록시가 아직 accept()로 꺼내지 않은 연결 수.
#      대기(LISTEN) 소켓의 Recv-Q 값이며, ss 명령으로 0.1초마다 읽는다.
#      늘 차 있으면 "연결이 메인 스레드를 기다린다", 자주 비면 "메인 스레드가 연결을 기다린다".
#   ② 메인 스레드의 시간 나누기 (/proc/PID/task/PID/schedstat: 나노초 단위 누적값 두 개)
#      CPU 위에서 실행 / 실행 대기(일할 준비는 됐는데 CPU 차례를 기다림) / 잠듦(나머지 전부:
#      accept에서 새 연결을 기다리거나, 커널 안의 락 등을 기다림)
#   ③ 문맥 교환 (/proc/PID/task/PID/status): 자발적(잠들면서 CPU를 내놓음) / 비자발적(CPU를 빼앗김)
# 메인 스레드의 스레드 번호(TID)는 프로세스 번호(PID)와 같다 (프로세스가 처음 만든 스레드).
#
# 결과: 화면의 표와 판정, results/diag_날짜_시각.csv (동시 연결 수마다 한 줄)
set -u
cd "$(dirname "$0")/.." || exit 1

CONCS=${CONCS:-"8 64"}
DUR=${DUR:-15}
WARM=${WARM:-2}
SHARDS=${SHARDS:-1}
KEYS=${KEYS:-200}
ZIPF=${ZIPF:-0.8}
DATA=${COOP_DATA:-$HOME/coop-data}
SMALL="$DATA/www-small"
LOGDIR="$DATA/logs"
PROXY_PORT=18620
ORIGIN_PORT=19620

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

if ! [[ "$DUR" =~ ^[0-9]+$ ]] || [ "$DUR" -lt 4 ]; then
    echo "DUR은 4 이상의 정수(초)여야 합니다 (입력값: $DUR)"; exit 1
fi
if ! [[ "$WARM" =~ ^[0-9]+$ ]]; then
    echo "WARM은 0 이상의 정수(초)여야 합니다 (입력값: $WARM)"; exit 1
fi
for c in $CONCS; do
    [[ "$c" =~ ^[1-9][0-9]*$ ]] || { echo "CONCS에 잘못된 값: $c"; exit 1; }
done
if ! command -v ss > /dev/null 2>&1; then
    echo "ss 명령이 없습니다: sudo apt install iproute2"; exit 1
fi

# ---------------------------------------------------------------- CPU 나누기 (exp_lock.sh와 같은 방식)
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
# 이 스크립트 자신을 부하 도구 쪽 CPU에 묶는다. 측정 중에 도는 ss·date도, 여기서 띄우는
# 원서버·loadc도 이 설정을 물려받으므로 프록시의 CPU를 건드리지 않는다.
# 프록시만 taskset으로 프록시 CPU를 따로 지정해 띄운다.
if [ -n "$LOAD_CPUS" ]; then
    taskset -cp "$LOAD_CPUS" $$ > /dev/null || { echo "taskset 실패 (LOAD_CPUS=$LOAD_CPUS)"; exit 1; }
fi

# ---------------------------------------------------------------- 준비
echo "[준비] release 빌드"
make -s MODE=release all loadc || { echo "빌드 실패"; exit 1; }
BIN=build/release/proxy
LC=build/release/loadc
if [ ! -f "$SMALL/manifest.json" ]; then
    echo "[준비] 1KB 객체 200개 생성 → $SMALL"
    python3 tools/gen_files.py --dir "$SMALL" --min 1024 --max 1024 > /dev/null || exit 1
fi
mkdir -p results "$LOGDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
CSV="results/diag_$STAMP.csv"
TMP=$(mktemp -d)
ORIGIN_PID=""
PP=""
LPID=""
cleanup() {
    [ -n "$LPID" ] && kill -TERM "$LPID" 2>/dev/null
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
    echo "포트 $1 이 열리지 않았습니다 ($LOGDIR/diag_*.log 확인)"
    exit 1
}
for p in $ORIGIN_PORT $PROXY_PORT; do
    if nc -z 127.0.0.1 "$p" 2>/dev/null; then
        echo "포트 $p 를 이미 다른 프로그램이 쓰고 있습니다 (ss -ltnp | grep $p 로 확인 후 종료하세요)"
        exit 1
    fi
done

# 메인 스레드의 누적값: 시각(ns) 실행(ns) 실행대기(ns) 자발적 비자발적  (schedstat이 없으면 실행·대기는 -1)
snap() {
    local t run=-1 wait=-1 vol nv
    t=$(date +%s%N)
    [ -r "/proc/$PP/task/$PP/schedstat" ] && read -r run wait _ < "/proc/$PP/task/$PP/schedstat"
    vol=$(sed -n 's/^voluntary_ctxt_switches:[[:space:]]*//p' "/proc/$PP/task/$PP/status")
    nv=$(sed -n 's/^nonvoluntary_ctxt_switches:[[:space:]]*//p' "/proc/$PP/task/$PP/status")
    echo "$t $run $wait ${vol:-0} ${nv:-0}"
}

WIN=$((DUR - 2))
NP=$(python3 -c "
import sys
n = 0
for part in sys.argv[1].split(','):
    if part:
        a, _, b = part.partition('-'); n += int(b or a) - int(a) + 1
print(n)" "${PROXY_CPUS:-0-$(($(nproc) - 1))}")
echo "[준비] 프록시 CPU ${PROXY_CPUS:-전체} / 부하 도구·원서버·측정 CPU ${LOAD_CPUS:-전체}"
echo "[준비] 샤드 $SHARDS개, 동시 연결 $CONCS, 각각 예열 ${WARM}초 + 측정 ${DUR}초 (그 안의 ${WIN}초 동안 관찰)"
python3 tools/origin.py --port $ORIGIN_PORT --dir "$SMALL" --quiet > "$LOGDIR/diag_origin.log" 2>&1 &
ORIGIN_PID=$!
wait_port $ORIGIN_PORT
if [ -n "$PROXY_CPUS" ]; then
    taskset -c "$PROXY_CPUS" "$BIN" -n diag -p $PROXY_PORT -q -v 600 -s 64M -S "$SHARDS" > "$LOGDIR/diag_proxy.log" 2>&1 &
else
    "$BIN" -n diag -p $PROXY_PORT -q -v 600 -s 64M -S "$SHARDS" > "$LOGDIR/diag_proxy.log" 2>&1 &
fi
PP=$!
wait_port $PROXY_PORT
if [ "$(cat /proc/$PP/comm 2>/dev/null)" != "proxy" ]; then
    echo "프록시(pid $PP)가 제대로 뜨지 않았습니다 → $LOGDIR/diag_proxy.log"; exit 1
fi
[ -r "/proc/$PP/task/$PP/schedstat" ] || echo "※ 이 커널은 schedstat을 제공하지 않습니다: ②의 실행 대기·잠듦은 빼고 봅니다"

# ---------------------------------------------------------------- 측정
for c in $CONCS; do
    echo
    echo "[측정] 동시 $c: 부하 시작, 예열 뒤 ${WIN}초 동안 0.1초마다 accept 대기열을 읽습니다"
    "$LC" -x 127.0.0.1:$PROXY_PORT -o 127.0.0.1:$ORIGIN_PORT -k "$KEYS" -z "$ZIPF" -c "$c" -d "$DUR" -w "$WARM" \
        --prefill --pid "$PP" -q --json "$TMP/r_$c.json" > "$TMP/r_$c.txt" 2>&1 &
    LPID=$!
    sleep $((WARM + 1))                    # 미리 채우기(200개)와 예열이 끝나 측정 구간에 들어갈 때까지
    s0=$(snap)
    end=$(( $(date +%s%N) + WIN * 1000000000 ))
    : > "$TMP/q_$c.txt"
    while [ "$(date +%s%N)" -lt "$end" ]; do
        # LISTEN 소켓 한 줄: 상태 Recv-Q(대기열 길이) Send-Q(대기열 상한) 주소...
        ss -Hltn "sport = :$PROXY_PORT" | awk 'NR == 1 { print $2, $3 }' >> "$TMP/q_$c.txt"
        sleep 0.1
    done
    s1=$(snap)
    wait "$LPID"
    rc=$?
    LPID=""
    if [ $rc -eq 2 ] || [ ! -s "$TMP/r_$c.json" ]; then
        echo "  측정 실패 (동시 $c):"; sed 's/^/    /' "$TMP/r_$c.txt"; exit 1
    fi
    echo "$c $s0 $s1" > "$TMP/s_$c.txt"
done

# ---------------------------------------------------------------- 분석
python3 - "$TMP" "$CSV" "$CONCS" "$NP" <<'EOF'
import json, os, statistics, sys

tmp, csv_path, concs, ncpu = sys.argv[1], sys.argv[2], sys.argv[3].split(), int(sys.argv[4])
rows = []
for c in concs:
    r = json.load(open(os.path.join(tmp, f"r_{c}.json")))["result"]
    f = open(os.path.join(tmp, f"s_{c}.txt")).read().split()
    t0, run0, wait0, vol0, nv0, t1, run1, wait1, vol1, nv1 = map(int, f[1:11])
    wall = (t1 - t0) / 1e9
    rps = r["rps"]
    nreq = rps * wall                                   # 관찰 구간의 요청 수 (측정 구간 평균 처리량으로)
    q, limit = [], None
    for line in open(os.path.join(tmp, f"q_{c}.txt")):
        p = line.split()
        if len(p) == 2:
            q.append(int(p[0]))
            limit = int(p[1])
    cpu = r["cpu"]
    has_sched = run0 >= 0 and run1 >= 0
    on = (run1 - run0) / 1e9 / wall if has_sched else (cpu.get("proxy_main_cores") or 0)
    rq = (wait1 - wait0) / 1e9 / wall if has_sched else None
    sl = max(0.0, 1 - on - rq) if has_sched else None
    us = lambda frac: None if frac is None else frac * 1e6 / rps
    mean_ms = r["latency_ms"]["mean"]
    qm = statistics.mean(q) if q else 0.0
    row = {
        "conc": int(c), "rps": rps, "mean_ms": mean_ms, "samples": len(q),
        "q_mean": qm, "q_median": statistics.median(q) if q else 0, "q_max": max(q) if q else 0,
        "q_limit": limit or 0, "q_nonempty_pct": 100 * sum(1 for v in q if v > 0) / len(q) if q else 0.0,
        "q_wait_ms": 1000 * qm / rps if rps else 0.0,
        "main_on_pct": 100 * on, "main_runq_pct": None if rq is None else 100 * rq,
        "main_sleep_pct": None if sl is None else 100 * sl,
        "main_on_us": us(on), "main_runq_us": us(rq), "main_sleep_us": us(sl),
        "vol_per_1k": 1000 * (vol1 - vol0) / nreq if nreq else 0.0,
        "nonvol_per_1k": 1000 * (nv1 - nv0) / nreq if nreq else 0.0,
        "loader_cores": cpu["loader_cores"], "proxy_cores": cpu.get("proxy_cores"),
        "loadc_main_cores": cpu.get("proxy_main_cores"), "loadc_main_us": cpu.get("proxy_main_us_per_req"),
        "wall": wall, "has_sched": has_sched,
    }
    rows.append(row)

fmt = lambda v, s="{:.1f}": "-" if v is None else s.format(v)
for x in rows:
    print()
    print(f"[동시 {x['conc']}] 처리량 {x['rps']:,.0f} 요청/s, 평균 응답 {x['mean_ms']:.3f}ms "
          f"(관찰 {x['wall']:.1f}초, 대기열 표본 {x['samples']}개)")
    print(f"  ① accept 대기열 (아직 accept()로 꺼내지 않은 연결, 상한 {x['q_limit']}):")
    print(f"       평균 {x['q_mean']:.1f}개, 중앙값 {x['q_median']:g}, 최대 {x['q_max']}  |  비어 있지 않던 시간 {x['q_nonempty_pct']:.0f}%")
    if x["mean_ms"] > 0:
        print(f"       → 연결 하나가 accept를 기다린 평균 시간 ≈ {x['q_mean']:.1f}개 ÷ {x['rps']:,.0f}/s = {x['q_wait_ms']:.3f}ms "
              f"(평균 응답 시간의 {100 * x['q_wait_ms'] / x['mean_ms']:.0f}%, 리틀의 법칙)")
    print(f"  ② 메인 스레드의 시간 (관찰 시간 = 100%):")
    if x["has_sched"]:
        print(f"       CPU 위에서 실행 {x['main_on_pct']:.1f}%  |  실행 대기 {x['main_runq_pct']:.1f}%  |  잠듦 {x['main_sleep_pct']:.1f}%")
        print(f"       요청 하나당: 실행 {x['main_on_us']:.1f}us, 실행 대기 {x['main_runq_us']:.1f}us, 잠듦 {x['main_sleep_us']:.1f}us")
    else:
        print(f"       CPU 위에서 실행 {x['main_on_pct']:.1f}% (loadc 측정값, schedstat 없음)")
    if x["loadc_main_cores"] is not None:
        print(f"       (loadc가 따로 잰 메인 스레드: {x['loadc_main_cores']:.2f}코어, 요청당 {x['loadc_main_us']:.1f}us "
              f"← 위의 '실행'과 비슷해야 정상)")
    print(f"  ③ 메인 스레드의 문맥 교환 (요청 1000개당): 자발적(잠듦) {x['vol_per_1k']:.1f}회, "
          f"비자발적(CPU를 빼앗김) {x['nonvol_per_1k']:.1f}회")
    print(f"  CPU: 프록시 전체 {fmt(x['proxy_cores'], '{:.2f}')}코어 (받은 CPU {ncpu}개), 부하 도구 {x['loader_cores']:.2f}코어")

    busy = None if x["main_runq_pct"] is None else x["main_on_pct"] + x["main_runq_pct"]
    full, empty = x["q_nonempty_pct"] >= 90, x["q_nonempty_pct"] <= 20
    if x["proxy_cores"] is not None and x["proxy_cores"] >= 0.9 * ncpu:
        verdict = (f"프록시에 준 CPU {ncpu}개가 거의 다 찼습니다({x['proxy_cores']:.2f}코어). 병목은 특정 스레드가 아니라 "
                   "CPU 자체입니다. 메인 스레드의 실행 대기는 작업 스레드들과 CPU를 나눠 쓰느라 생긴 것입니다.")
    elif full and (busy is None or busy >= 90):
        how = "" if busy is None else (f" 메인 스레드는 쉬지 않습니다: 실행 {x['main_on_pct']:.0f}% + 실행 대기 {x['main_runq_pct']:.0f}%."
                                       " 실행 대기는 다른 스레드와 CPU를 나눠 쓰느라 늦어진 시간입니다.")
        verdict = "메인 스레드가 병목입니다 (확정). 연결이 늘 accept를 기다리며 줄 서 있고," + how
    elif full:
        verdict = (f"연결은 줄 서 있는데 메인 스레드가 관찰 시간의 {x['main_sleep_pct']:.0f}% 동안 잠들어 있습니다. "
                   "새 연결을 기다린 것이 아니라(대기열이 차 있으므로) 스레드를 만드는 과정의 커널 락 같은 곳에서 "
                   "막힌 것입니다. 병목은 여전히 메인 스레드의 경로입니다.")
    elif empty:
        verdict = (f"대기열이 자주 비어 있습니다(비어 있지 않던 시간 {x['q_nonempty_pct']:.0f}%). 메인 스레드는 연결을 "
                   "기다리며 쉬는 시간이 많으므로, 이 동시 연결 수에서는 메인 스레드가 병목이 아닙니다.")
    else:
        verdict = (f"대기열이 관찰 시간의 {x['q_nonempty_pct']:.0f}%만 차 있습니다. 메인 스레드가 부분적으로 병목입니다 "
                   "(연결이 몰릴 때만 줄이 생김).")
    print(f"  판정: {verdict}")
    x["verdict"] = verdict

cols = ["conc", "rps", "mean_ms", "samples", "q_mean", "q_median", "q_max", "q_limit", "q_nonempty_pct", "q_wait_ms",
        "main_on_pct", "main_runq_pct", "main_sleep_pct", "main_on_us", "main_runq_us", "main_sleep_us",
        "vol_per_1k", "nonvol_per_1k", "loader_cores", "proxy_cores", "loadc_main_cores", "loadc_main_us"]
with open(csv_path, "w") as fh:
    fh.write(",".join(cols) + "\n")
    for x in rows:
        fh.write(",".join("" if x[k] is None else (f"{x[k]:.4f}" if isinstance(x[k], float) else str(x[k]))
                          for k in cols) + "\n")
print()
print(f"결과 파일: {csv_path}")
EOF

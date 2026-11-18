#!/usr/bin/env bash
# 8단계 검증: 장시간 부하 — 클러스터 3노드에 오래 부하를 걸면서 노드마다 메모리(RSS)·열린 파일 수·스레드 수를 기록한다
#
# 사용법
#   ./scripts/soak.sh                        30분, 10초마다 기록
#   MIN=60 ./scripts/soak.sh                 60분
#   SEC=40 INTERVAL=2 ./scripts/soak.sh      짧게 (점검용)
# 바꿀 수 있는 값: MIN=30 (또는 초 단위 SEC)  INTERVAL=10  CONC=8  SIZE=2M  VALIDATE=5  ZIPF=0.8
#
# 조건: 객체 200개(1KB~512KB, 합 약 11.7MB, ~/coop-data/www), 노드당 캐시 2MB → 교체가 쉬지 않고 일어난다.
#       검증 주기 5초 → 5초가 지난 객체는 원서버에 조건부 GET을 보낸다 (304 또는 새 내용).
#       loadc가 진입 노드 3개에 돌아가며 보낸다 → HIT·MISS·FORWARD·교체·재검증이 모두 섞인다.
#       release 빌드. 새니타이저 없이 실제로 쓰는 모습 그대로 오래 돌리는 시험이다.
#
# 무엇을 보면 누수가 없다고 할 수 있나
#   열린 파일·스레드 수: 부하 전(노드를 띄운 직후)과 부하 뒤(연결이 모두 끝난 뒤)의 값이 같다.
#     연결마다 소켓과 스레드를 만들고 없애므로, 하나라도 빠뜨리면 요청 수만큼 쌓인다.
#   사용 중인 힙(/__stats의 memory.heap_in_use: malloc으로 받아 아직 free하지 않은 바이트):
#     처음에는 캐시가 차면서 늘지만 곧 평평해진다. 뒤쪽 절반의 기울기가 시간당 3MB 미만이면 "증가 없음".
#     (요청 수백만 개 동안 요청당 몇 바이트만 새도 시간당 수십 MB씩 는다)
#   RSS(프로세스가 실제로 차지한 물리 메모리)는 참고로 함께 기록한다. free한 메모리도 할당기(glibc malloc)가
#     다음을 위해 쥐고 있고, 스레드마다 다른 아레나(할당 구역)를 쓰며 크기가 제각각인 객체(1KB~512KB)를
#     오래 할당·해제하면 빈 공간이 조각나 RSS는 천천히 늘 수 있다. 이때 heap_system(할당기가 받아 둔 양)도
#     같이 늘고 heap_in_use는 그대로다 → 누수가 아니라 할당기의 보관분이다.
# 결과: results/soak_날짜_시각.csv (기록 1번 × 노드 = 한 줄), .env
#       그래프: python3 tools/plot_results.py soak results/soak_날짜_시각.csv
set -u
cd "$(dirname "$0")/.." || exit 1

MIN=${MIN:-30}
TOTAL=${SEC:-$((MIN * 60))}
INTERVAL=${INTERVAL:-10}
CONC=${CONC:-8}
SIZE=${SIZE:-2M}
VALIDATE=${VALIDATE:-5}
ZIPF=${ZIPF:-0.8}
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
PORTS=(18801 18802 18803)
ORIGIN_PORT=19800

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

for v in TOTAL INTERVAL CONC VALIDATE; do
    [[ "${!v}" =~ ^[1-9][0-9]*$ ]] || { echo "$v는 1 이상의 정수여야 합니다 (입력값: ${!v})"; exit 1; }
done
if [ "$TOTAL" -lt $((INTERVAL * 3)) ]; then
    echo "실행 시간(${TOTAL}초)이 기록 간격(${INTERVAL}초)의 3배보다 짧습니다"; exit 1
fi

echo "[준비] release 빌드"
make -s MODE=release all loadc || { echo "빌드 실패"; exit 1; }
BIN=build/release/proxy
LC=build/release/loadc
if [ ! -f "$WWW/manifest.json" ]; then
    echo "[준비] 테스트 파일 생성 → $WWW"
    python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
fi
mkdir -p results "$LOGDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
CSV="results/soak_$STAMP.csv"
ENVF="results/soak_$STAMP.env"
TMP=$(mktemp -d)
CONF="$TMP/cluster.conf"
printf 'node1 127.0.0.1 %d\nnode2 127.0.0.1 %d\nnode3 127.0.0.1 %d\n' "${PORTS[@]}" > "$CONF"
PROXIES=$(printf '127.0.0.1:%d,' "${PORTS[@]}"); PROXIES=${PROXIES%,}
NODE_PIDS=()
ORIGIN_PID=""
LPID=""
cleanup() {
    [ -n "$LPID" ] && kill -TERM "$LPID" 2>/dev/null
    for p in "${NODE_PIDS[@]}"; do kill -TERM "$p" 2>/dev/null; done
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
    echo "포트 $1 이 열리지 않았습니다 ($LOGDIR/soak_*.log 확인)"
    exit 1
}
for p in $ORIGIN_PORT "${PORTS[@]}"; do
    if nc -z 127.0.0.1 "$p" 2>/dev/null; then
        echo "포트 $p 를 이미 다른 프로그램이 쓰고 있습니다 (ss -ltnp | grep $p 로 확인 후 종료하세요)"
        exit 1
    fi
done

NOBJ=$(python3 -c "import json, sys; print(len(json.load(open(sys.argv[1]))['objects']))" "$WWW/manifest.json")
OBJMB=$(python3 -c "import json, sys; print(round(sum(o['size'] for o in json.load(open(sys.argv[1]))['objects']) / 1048576, 1))" "$WWW/manifest.json")
{
    echo "date=$(date '+%Y-%m-%d %H:%M:%S')"
    echo "commit=$(git rev-parse --short HEAD 2>/dev/null)$(git diff --quiet 2>/dev/null || echo '+수정됨')"
    echo "cpu_model=$(lscpu | sed -n 's/^Model name: *//p')"
    echo "total_sec=$TOTAL interval=$INTERVAL conc=$CONC size=$SIZE validate=$VALIDATE zipf=$ZIPF"
    echo "objects=$NOBJ objects_mb=$OBJMB nodes=3"
    echo "proxy_args=-n nodeK -C <노드 목록> -q -v $VALIDATE -s $SIZE"
} > "$ENVF"
echo "t_sec,phase,node,rss_kb,heap_in_use_kb,heap_system_kb,fds,threads,active,requests,cache_objects,cache_bytes,evictions,origin_requests" > "$CSV"

# 노드마다 한 줄씩 기록한다. /proc를 먼저 읽고 /__stats를 나중에 부른다:
# /__stats 요청 자체도 연결 하나(소켓 + 스레드)이므로, 순서가 반대면 그 연결이 함께 세어진다.
sample() {   # sample 단계(before|load|after) 경과초
    python3 - "$CSV" "$1" "$2" "${NODE_PIDS[*]}" "${PORTS[*]}" <<'EOF'
import json, os, sys, urllib.request

csv_path, phase, t = sys.argv[1], sys.argv[2], sys.argv[3]
pids, ports = sys.argv[4].split(), sys.argv[5].split()
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
out = []
for k, (pid, port) in enumerate(zip(pids, ports)):
    st = {}
    try:
        for line in open(f"/proc/{pid}/status"):
            key, _, val = line.partition(":")
            st[key] = val.split()
        rss, thr = int(st["VmRSS"][0]), int(st["Threads"][0])
        fds = len(os.listdir(f"/proc/{pid}/fd"))
    except (OSError, KeyError):
        rss = thr = fds = -1                       # 노드가 죽었다
    try:
        d = json.load(opener.open(f"http://127.0.0.1:{port}/__stats", timeout=5))
        mem = d.get("memory", {})
        heap = [mem.get("heap_in_use", -1024) // 1024, mem.get("heap_system", -1024) // 1024]
        stats = [d["active_connections"] - 1,      # 지금 이 /__stats 연결은 빼고
                 d["requests"]["client"] + d["requests"]["peer"], d["cache"]["objects"], d["cache"]["bytes"],
                 d["cache"]["evictions"], d["origin_requests"]]
    except Exception:
        heap, stats = [-1] * 2, [-1] * 6
    out.append(",".join(map(str, [t, phase, f"node{k + 1}", rss] + heap + [fds, thr] + stats)))
with open(csv_path, "a") as fh:
    fh.write("\n".join(out) + "\n")
EOF
}
show() {     # 마지막 기록 3줄을 한 줄로
    tail -3 "$CSV" | awk -F, -v t="$1" -v total="$TOTAL" '
        { r[NR] = sprintf("%s RSS %5.1fMB 힙 %4.1fMB fd %3d 스레드 %3d", $3, $4 / 1024, $5 / 1024, $7, $8); req += $10 }
        END { printf "  [%3d:%02d / %d:%02d] %s | %s | %s | 처리한 요청 %d\n",
                     t / 60, t % 60, total / 60, total % 60, r[1], r[2], r[3], req }'
}

echo "[준비] 노드 3개 (캐시 $SIZE, 검증 주기 ${VALIDATE}초), 객체 ${NOBJ}개(${OBJMB}MB), 동시 $CONC"
echo "[준비] $((TOTAL / 60))분 $((TOTAL % 60))초 동안 부하, ${INTERVAL}초마다 기록 → $CSV"
python3 tools/origin.py --port $ORIGIN_PORT --dir "$WWW" --quiet > "$LOGDIR/soak_origin.log" 2>&1 &
ORIGIN_PID=$!
wait_port $ORIGIN_PORT
for k in 0 1 2; do
    "$BIN" -n "node$((k + 1))" -C "$CONF" -q -v "$VALIDATE" -s "$SIZE" > "$LOGDIR/soak_node$((k + 1)).log" 2>&1 &
    NODE_PIDS+=($!)
done
for p in "${PORTS[@]}"; do wait_port "$p"; done
sleep 0.5

BASE_THR=()
for p in "${NODE_PIDS[@]}"; do BASE_THR+=("$(sed -n 's/^Threads:[[:space:]]*//p' "/proc/$p/status")"); done
sample before 0
show 0
T0=$(date +%s%N)
"$LC" -x "$PROXIES" -o 127.0.0.1:$ORIGIN_PORT -k "$NOBJ" -z "$ZIPF" -c "$CONC" -d "$TOTAL" -q \
    --json "$TMP/load.json" > "$TMP/load.txt" 2>&1 &
LPID=$!
next=$INTERVAL
every=$(( (60 + INTERVAL - 1) / INTERVAL ))     # 화면에는 약 1분마다 한 번
count=0
while kill -0 "$LPID" 2>/dev/null; do
    el=$(( ($(date +%s%N) - T0) / 1000000000 ))
    if [ "$el" -ge "$next" ] && [ "$el" -le "$TOTAL" ]; then
        sample load "$el"
        count=$((count + 1))
        [ $((count % every)) -eq 0 ] && show "$el"
        next=$((next + INTERVAL))
    fi
    sleep 0.2
done
wait "$LPID"
lrc=$?
LPID=""
# 부하가 끝난 뒤: 진행 중이던 연결(스레드)이 모두 끝나길 최대 10초 기다렸다가 기록
for _ in $(seq 1 50); do
    busy=0
    for k in 0 1 2; do
        [ "$(sed -n 's/^Threads:[[:space:]]*//p' "/proc/${NODE_PIDS[$k]}/status" 2>/dev/null)" = "${BASE_THR[$k]}" ] || busy=1
    done
    [ $busy -eq 0 ] && break
    sleep 0.2
done
el=$(( ($(date +%s%N) - T0) / 1000000000 ))
sample after "$el"
echo "  부하 끝 (연결이 모두 끝난 뒤):"
show "$el"

# 노드를 정상 종료시키고 종료 코드·종료 기록을 확인한다
node_rc=""
for k in 0 1 2; do
    kill -TERM "${NODE_PIDS[$k]}" 2>/dev/null
    wait "${NODE_PIDS[$k]}"
    node_rc="$node_rc $?"
done
NODE_PIDS=()
kill -TERM "$ORIGIN_PID" 2>/dev/null; wait "$ORIGIN_PID" 2>/dev/null; ORIGIN_PID=""

python3 - "$CSV" "$TMP/load.json" "$TOTAL" "$lrc" "$node_rc" "$LOGDIR" <<'EOF'
import csv, json, os, sys
from collections import defaultdict

csv_path, load_json, total, lrc, node_rc, logdir = sys.argv[1:7]
total, lrc, node_rc = int(total), int(lrc), node_rc.split()
rows = list(csv.DictReader(open(csv_path)))
by = defaultdict(list)
for r in rows:
    by[r["node"]].append(r)
ok = True

print()
print("[부하 도구]")
if os.path.exists(load_json) and os.path.getsize(load_json) > 0:
    d = json.load(open(load_json))["result"]
    labels = " · ".join(f"{k} {v:,}" for k, v in d["labels"].items() if v)
    print(f"  요청 {d['requests']:,}개, 실패 {d['failed']}개, 평균 {d['rps']:,.0f} 요청/s, 응답 시간 평균 {d['latency_ms']['mean']:.2f}ms")
    print(f"  결과(마지막 X-Cache): {labels}")
    if d["failed"]:
        errs = " · ".join(f"{k} {v}" for k, v in d["errors"].items() if v)
        print(f"  ※ 실패한 요청: {errs}")
        ok = False
else:
    print(f"  ※ 결과가 없습니다 (종료 코드 {lrc})")
    ok = False

print()
print("[노드별 판정]")
short = total < 600
for k, node in enumerate(sorted(by)):
    rs = by[node]
    before = next(r for r in rs if r["phase"] == "before")
    after = [r for r in rs if r["phase"] == "after"][-1]
    load = [r for r in rs if r["phase"] == "load" and int(r["rss_kb"]) >= 0]
    fb, fa, tb, ta = int(before["fds"]), int(after["fds"]), int(before["threads"]), int(after["threads"])
    fd_ok, th_ok = fa == fb and fa >= 0, ta == tb and ta >= 0
    ok &= fd_ok and th_ok
    print(f"  {node}: 열린 파일 부하 전 {fb} → 부하 뒤 {fa} {'(같음 ✓)' if fd_ok else '(다름 ✗ 누수 의심)'}"
          f" | 스레드 {tb} → {ta} {'(같음 ✓)' if th_ok else '(다름 ✗)'}"
          f" | 부하 중 최대 열린 파일 {max(int(r['fds']) for r in load) if load else '-'}")
    # 뒤쪽 절반의 기울기 (최소제곱 직선, MB/시간)
    def slope(field):
        pts = [(int(r["t_sec"]), int(r[field]) / 1024) for r in load if int(r["t_sec"]) >= total / 2 and int(r[field]) >= 0]
        if len(pts) < 3:
            return None
        n = len(pts)
        mx, my = sum(x for x, _ in pts) / n, sum(y for _, y in pts) / n
        sxx = sum((x - mx) ** 2 for x, _ in pts)
        return sum((x - mx) * (y - my) for x, y in pts) / sxx * 3600 if sxx else 0.0
    mb = lambda r, f: int(r[f]) / 1024
    note = " (실행이 10분보다 짧아 참고용)" if short else ""
    s_use, s_sys, s_rss = slope("heap_in_use_kb"), slope("heap_system_kb"), slope("rss_kb")
    if s_use is None or not load:
        print("         메모리: 기록이 적어 기울기를 구하지 않았습니다")
    else:
        use = [mb(r, "heap_in_use_kb") for r in load]
        stable = s_use < 3.0
        if not short:
            ok &= stable
        print(f"         사용 중인 힙: 부하 중 최대 {max(use):.1f}MB, 마지막 {use[-1]:.1f}MB | 뒤쪽 절반 기울기 "
              f"{s_use:+.2f}MB/시간 {'(증가 없음 ✓)' if stable else '(증가 경향 ✗ 누수 의심)'}{note}")
        print(f"         참고) RSS {mb(before, 'rss_kb'):.1f} → 최대 {max(mb(r, 'rss_kb') for r in load):.1f}MB "
              f"(기울기 {s_rss:+.1f}MB/시간), 할당기가 받아 둔 힙 마지막 {mb(load[-1], 'heap_system_kb'):.1f}MB "
              f"(기울기 {s_sys:+.1f}MB/시간)")
    rc = node_rc[k] if k < len(node_rc) else "?"
    log = os.path.join(logdir, f"soak_{node}.log")
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    clean = rc == "0" and "종료: 연결" in text and "끝나지 않은 연결" not in text
    ok &= clean
    print(f"         정상 종료 {'✓ (종료 코드 0, 연결을 모두 정리한 뒤 캐시 해제)' if clean else f'✗ (종료 코드 {rc}, {log} 확인)'}")
print()
print("판정: " + ("누수 징후 없음 — 파일·스레드는 부하 전 상태로 돌아왔고, 실패한 요청이 없습니다" +
                  ("" if short else ", 사용 중인 힙도 늘지 않습니다") if ok else "확인이 필요한 항목이 있습니다 (✗ 표시)"))
print("      RSS가 사용 중인 힙보다 크게·천천히 느는 것은 할당기가 free된 메모리를 쥐고 있기 때문입니다 (누수 아님).")
print(f"결과 파일: {csv_path}")
print(f"그래프: python3 tools/plot_results.py soak {csv_path}")
sys.exit(0 if ok else 1)
EOF

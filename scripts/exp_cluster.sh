#!/usr/bin/env bash
# 7단계 실험 ②: 독립 프록시 3개 대 협력형 클러스터 3노드 — 노드 하나의 캐시 용량별
#
# 바꿀 수 있는 값: SIZES="512K 1M 2M 4M 8M 16M"  N=6000  WARMUP=3000  CONC=16  DELAY=0.05  ZIPF=0.8
#                  REPEAT=2  SEED=2026  RATE=0
#   RATE: 초당 요청 수 상한 (0 = 제한 없음, 8단계). loadgen.py는 Python이라 초당 약 2,000개가 한계여서,
#         히트율이 높아 요청이 빨리 끝나는 조건에서는 부하 생성기 자신이 줄을 세워 응답 시간이 부풀려진다.
#         RATE=250처럼 한계보다 한참 낮게 주면 "HIT 요청만의 응답 시간"이 프록시와 노드 간 전달만 반영한다
#         (예열 요청에도 같은 상한이 걸리므로 측정 1회에 (N + WARMUP) ÷ RATE초가 걸린다)
#
# 측정 조건
#   - 원서버 응답 지연 50ms: 멀리 있는 원서버 흉내. 원서버에 다녀오는 비용이 응답 시간에 드러난다
#   - 객체: ~/coop-data/www/obj 200개 (1KB~512KB, 합계 약 12MB)
#   - 부하 생성기(loadgen.py)가 진입 노드 3개에 요청을 돌아가며 보낸다 (클라이언트가 노드에 고르게 퍼진 상황).
#     같은 시드 → 두 방식에 똑같은 요청열. 응답 내용은 CRC32로 확인한다
#   - 예열 요청은 측정에서 뺀다 → 캐시가 어느 정도 찬 "평상시"를 잰다
#   - 조건마다 노드를 새로 띄운다 (빈 캐시에서 시작). 반복마다 두 방식의 순서를 바꾼다
#
# 결과: results/cluster_날짜_시각.csv (측정 1회 = 한 줄), .env (실험 환경), 마지막에 요약 표
#       그래프: python3 tools/plot_results.py cluster results/cluster_날짜_시각.csv
set -u
cd "$(dirname "$0")/.." || exit 1

if [ "${QUICK:-0}" = 1 ]; then
    SIZES=${SIZES:-"1M 4M"}; N=${N:-1500}; WARMUP=${WARMUP:-500}; REPEAT=${REPEAT:-1}
else
    SIZES=${SIZES:-"512K 1M 2M 4M 8M 16M"}; N=${N:-6000}; WARMUP=${WARMUP:-3000}; REPEAT=${REPEAT:-2}
fi
CONC=${CONC:-16}
DELAY=${DELAY:-0.05}
ZIPF=${ZIPF:-0.8}
SEED=${SEED:-2026}
RATE=${RATE:-0}
[[ "$RATE" =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "RATE는 0 이상의 수여야 합니다 (입력값: $RATE)"; exit 1; }
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
ORIGIN_PORT=19700
PORTS=(18701 18702 18703)

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

echo "[준비] release 빌드"
make -s MODE=release all || { echo "빌드 실패"; exit 1; }
BIN=build/release/proxy
if [ ! -f "$WWW/manifest.json" ]; then
    echo "[준비] 테스트 파일 생성 → $WWW"
    python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
fi
mkdir -p results "$LOGDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
CSV="results/cluster_$STAMP.csv"
ENVF="results/cluster_$STAMP.env"
TMP=$(mktemp -d)
CONF="$TMP/cluster.conf"
printf 'node1 127.0.0.1 %d\nnode2 127.0.0.1 %d\nnode3 127.0.0.1 %d\n' "${PORTS[@]}" > "$CONF"
PROXIES=$(printf '127.0.0.1:%d,' "${PORTS[@]}"); PROXIES=${PROXIES%,}
NODE_PIDS=()
ORIGIN_PID=""

stop_nodes() {
    for p in "${NODE_PIDS[@]}"; do kill -TERM "$p" 2>/dev/null; done
    for p in "${NODE_PIDS[@]}"; do wait "$p" 2>/dev/null; done
    NODE_PIDS=()
}
cleanup() {
    stop_nodes
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
    echo "포트 $1 이 열리지 않았습니다 ($LOGDIR/exp_cluster_*.log 확인)"
    exit 1
}
for p in $ORIGIN_PORT "${PORTS[@]}"; do
    if nc -z 127.0.0.1 "$p" 2>/dev/null; then
        echo "포트 $p 를 이미 다른 프로그램이 쓰고 있습니다 (ss -ltnp | grep $p 로 확인 후 종료하세요)"
        exit 1
    fi
done

read -r NOBJ OBJMB < <(python3 -c "
import json, sys
m = json.load(open(sys.argv[1]))['objects']
print(len(m), round(sum(o['size'] for o in m) / 1048576, 1))" "$WWW/manifest.json")
runs=$(( REPEAT * $(wc -w <<< "$SIZES") * 2 ))
{
    echo "date=$(date '+%Y-%m-%d %H:%M:%S')"
    echo "commit=$(git rev-parse --short HEAD 2>/dev/null)$(git diff --quiet 2>/dev/null || echo '+수정됨')"
    echo "cpu_model=$(lscpu | sed -n 's/^Model name: *//p')"
    echo "sizes=$SIZES requests=$N warmup=$WARMUP conc=$CONC delay=$DELAY zipf=$ZIPF repeat=$REPEAT seed=$SEED rate=$RATE"
    echo "objects=$NOBJ objects_mb=$OBJMB nodes=3"
    echo "proxy_args=-q -v 600 -s <용량> (독립: -p 포트 / 클러스터: -C 노드목록)"
} > "$ENVF"
echo "rep,mode,size,size_bytes,requests,failed,hit_ratio,origin_requests,origin_per_1000,mean_ms,p50_ms,p90_ms,p99_ms,rps,forwarded,objects,evictions,loadgen_rc,hit_mean_ms,miss_mean_ms" > "$CSV"

echo "[준비] 노드 3개, 객체 ${NOBJ}개(${OBJMB}MB), 원서버 지연 ${DELAY}초, 요청 ${N}개(예열 ${WARMUP}개 제외), 동시 ${CONC}"
echo "[준비] 측정 ${runs}회$(python3 -c "
import sys
r, n, w = float(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
print(f', 요청 속도 상한 초당 {r:g}개 (1회에 약 {(n + w) / r:.0f}초)' if r > 0 else '')" "$RATE" "$N" "$WARMUP")"
python3 tools/origin.py --port $ORIGIN_PORT --dir "$WWW" --delay "$DELAY" --quiet > "$LOGDIR/exp_cluster_origin.log" 2>&1 &
ORIGIN_PID=$!
wait_port $ORIGIN_PORT

i=0
for rep in $(seq 1 "$REPEAT"); do
    modes="independent cluster"
    [ $((rep % 2)) -eq 0 ] && modes="cluster independent"
    for size in $SIZES; do
        for mode in $modes; do
            i=$((i + 1))
            for k in 0 1 2; do
                name="node$((k + 1))"
                if [ "$mode" = cluster ]; then
                    "$BIN" -n "$name" -C "$CONF" -q -v 600 -s "$size" > "$LOGDIR/exp_cluster_$name.log" 2>&1 &
                else
                    "$BIN" -n "$name" -p "${PORTS[$k]}" -q -v 600 -s "$size" > "$LOGDIR/exp_cluster_$name.log" 2>&1 &
                fi
                NODE_PIDS+=($!)
            done
            for p in "${PORTS[@]}"; do wait_port "$p"; done

            python3 tools/loadgen.py --proxies "$PROXIES" --origin "127.0.0.1:$ORIGIN_PORT" \
                --manifest "$WWW/manifest.json" -n "$N" --warmup "$WARMUP" -c "$CONC" --zipf "$ZIPF" \
                --rate "$RATE" --seed "$SEED" --stats --json "$TMP/r.json" --csv "$TMP/req.csv" -q > "$TMP/r.txt" 2>&1
            rc=$?
            nodes=$(for p in "${PORTS[@]}"; do curl -s "http://127.0.0.1:$p/__stats"; echo; done)
            stop_nodes
            if [ ! -s "$TMP/r.json" ]; then
                echo "  측정 실패 ($mode, $size):"; sed 's/^/    /' "$TMP/r.txt"; exit 1
            fi
            row=$(python3 - "$TMP/r.json" "$rep" "$mode" "$size" "$rc" "$nodes" "$TMP/req.csv" <<'EOF'
import csv, json, statistics, sys
d = json.load(open(sys.argv[1]))
rep, mode, size, rc, nodes = sys.argv[2:7]
# 요청별 기록에서 결과(마지막 X-Cache)가 HIT인 것과 MISS인 것의 평균 응답 시간
by = {"HIT": [], "MISS": []}
with open(sys.argv[7], newline="") as fh:
    for r in csv.DictReader(fh):
        if r["final"] in by and not r["error"]:
            by[r["final"]].append(float(r["latency_ms"]))
hm = statistics.mean(by["HIT"]) if by["HIT"] else None
mm = statistics.mean(by["MISS"]) if by["MISS"] else None
mult = {"K": 1024, "M": 1048576, "G": 1073741824}
nbytes = int(float(size[:-1]) * mult[size[-1].upper()]) if size[-1].upper() in mult else int(size)
s, L = d["result"], d["result"]["latency_ms"]
origin = (d.get("origin") or {}).get("total")
st = [json.loads(x) for x in nodes.split("\n") if x.strip()]
objs = sum(x["cache"]["objects"] for x in st)
ev = sum(x["cache"]["evictions"] for x in st)
f = lambda v: "" if v is None else f"{v:.4f}"
print(",".join([rep, mode, size, str(nbytes), str(s["requests"]), str(s["failed"]), f(s["hit_ratio"]),
                "" if origin is None else str(origin),
                "" if origin is None else f"{1000 * origin / s['requests']:.2f}",
                f(L["mean"]), f(L["p50"]), f(L["p90"]), f(L["p99"]), f"{s['rps']:.1f}",
                str(s["forwarded"]), str(objs), str(ev), rc, f(hm), f(mm)]))
EOF
)
            echo "$row" >> "$CSV"
            IFS=, read -r _ _ _ _ _ failed hit orig o1k mean p50 _ p99 _ fwd objs ev lrc hmean _ <<< "$row"
            printf '  [%2d/%d] 반복 %d  %-11s 노드당 %4s → 히트율 %5.1f%%  원서버 %5s개(1000개당 %6s)  평균 %6.2fms  HIT만 %5.2fms  p99 %6.1fms  객체 %3s개  교체 %4s회%s\n' \
                   "$i" "$runs" "$rep" "$mode" "$size" "$(python3 -c "print(100 * $hit)")" "$orig" "$o1k" "$mean" "${hmean:-0}" "$p99" "$objs" "$ev" \
                   "$([ "$failed" != 0 ] && echo "  ※ 실패 $failed")$([ "$lrc" = 3 ] && echo "  ※ 노드 통계 확인 불일치(loadgen 종료 코드 3)")"
        done
    done
done

echo
echo "결과 파일: $CSV"
echo "실험 환경: $ENVF"
python3 tools/cluster_summary.py "$CSV"
echo
echo "그래프: python3 tools/plot_results.py cluster $CSV"

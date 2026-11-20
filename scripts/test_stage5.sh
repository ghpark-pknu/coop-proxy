#!/usr/bin/env bash
# 5단계(캐시 락 전략: 전역 mutex / 샤딩) 완료 기준 자동 점검
#
# 완료 기준
#   - 캐시 단위 테스트(cache_test) 통과: 샤드 고르기, 용량 나누기, 샤드별 LRU, 동시 접근
#   - 캐시가 넘치지 않으면 -S 1과 -S 8이 같은 요청열에 같은 결과 (HIT·MISS·원서버 요청·락 획득 수)
#   - 동시 부하에서 실패 없음, 락 획득 수 = 요청 수 × 2 (빠짐없이 셈)
#   - 클러스터 노드를 -S 8로 띄워도 4단계의 정확성(원서버 요청 = 예측값, 단일 사본)이 그대로
#   - 새니타이저 경고 0개
#
# 시험용 포트: 독립 프록시 18481~18486, 클러스터 18491~18493, 원서버 19400
set -u
cd "$(dirname "$0")/.." || exit 1

MODE=${MODE:-debug}
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
ORIGIN_PORT=19400
O="127.0.0.1:$ORIGIN_PORT"

unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY all_proxy ALL_PROXY

pass=0
fail=0
ok() { printf '  \033[32m✓\033[0m %s\n' "$1"; pass=$((pass + 1)); }
ng() { printf '  \033[31m✗\033[0m %s\n' "$1"; fail=$((fail + 1)); }
expect() {      # 설명, 기대값, 실제값
    if [ "$3" = "$2" ]; then ok "$1 → $3"; else ng "$1 → ${3:-없음} (기대값 $2)"; fi
}
truth() {       # truth 설명 파이썬식 → 식이 참이면 통과
    if python3 -c "import sys; sys.exit(0 if ($2) else 1)" 2>/dev/null; then ok "$1"; else ng "$1"; fi
}
jget() {        # jget 파일 파이썬식 (d = JSON) → 값
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print($2)" "$1" 2>/dev/null
}
sget() {        # sget 포트 파이썬식 (d = /__stats JSON) → 값
    curl -s "http://127.0.0.1:$1/__stats" | python3 -c "import json,sys; d=json.load(sys.stdin); print($2)" 2>/dev/null
}

# ---------------------------------------------------------------- 준비
echo "[준비] 빌드 (MODE=$MODE)"
make -s MODE="$MODE" all ctest || { echo "빌드 실패"; exit 1; }
BIN="build/$MODE/proxy"

if [ ! -f "$WWW/manifest.json" ]; then
    echo "[준비] 테스트 파일 생성 → $WWW"
    python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
fi
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/test5_*.log
TMP=$(mktemp -d)

declare -A PID=()
RCS=""                  # 종료한 프록시들의 종료 코드를 이어 붙인 문자열
ORIGIN_PID=""
LG=(python3 tools/loadgen.py --origin "$O" --manifest "$WWW/manifest.json" -q)

wait_port() {
    for _ in $(seq 1 100); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "  포트 $1 이 열리지 않았습니다. $LOGDIR/test5_*.log 를 확인하세요."
    return 1
}
start_proxy() {     # start_proxy 이름 포트 [옵션...] → 독립 프록시 (-q: 기록 생략, -v 600: 재검증 없음)
    local name=$1 port=$2
    shift 2
    "$BIN" -n "$name" -p "$port" -q -v 600 "$@" >> "$LOGDIR/test5_$name.log" 2>&1 &
    PID[$name]=$!
    wait_port "$port"
}
stop_proc() {       # stop_proc 이름 → SIGTERM으로 정상 종료시키고 종료 코드를 모은다
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

python3 tools/origin.py --port $ORIGIN_PORT --dir "$WWW" --quiet > "$LOGDIR/test5_origin.log" 2>&1 &
ORIGIN_PID=$!
wait_port $ORIGIN_PORT || exit 1

# ---------------------------------------------------------------- 점검
echo "[0] 캐시 단위 테스트 (build/$MODE/cache_test)"
if "build/$MODE/cache_test" > "$TMP/ct.txt" 2>&1; then
    ok "샤드 고르기·용량 나누기·샤드별 LRU·동등성·스레드 8개 동시 접근 모두 통과"
else
    ng "cache_test 실패"
    sed 's/\x1b\[[0-9;]*m//g' "$TMP/ct.txt" | grep -E "✗|ERROR|WARNING" | head -5 | sed 's/^/      /'
fi
san=$(grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:' "$TMP/ct.txt")
expect "  단위 테스트의 새니타이저 경고 수" 0 "$san"

echo "[1] -S 옵션 검사"
for bad in 0 65 abc; do
    "$BIN" -p 18489 -S "$bad" > "$TMP/opt.txt" 2>&1
    rc=$?
    if [ $rc -eq 2 ] && grep -q "잘못된 샤드 수" "$TMP/opt.txt"; then
        ok "-S $bad → 거부 (종료 코드 2)"
    else
        ng "-S $bad → 종료 코드 $rc, 출력: $(head -1 "$TMP/opt.txt")"
    fi
done
"$BIN" -p 18489 -S 8 -s 4M > "$TMP/opt.txt" 2>&1
rc=$?
if [ $rc -eq 2 ] && grep -q "샤드 하나의 용량(512.0KB = 4.0MB ÷ 8)이 객체 상한(1.0MB)보다 작습니다" "$TMP/opt.txt" \
   && grep -q -- "-s를 8.0MB 이상으로 늘리거나, -o를 512.0KB 이하로" "$TMP/opt.txt"; then
    ok "-S 8 -s 4M (기본 객체 상한 1MB) → 거부하고 고칠 값을 안내 (종료 코드 2)"
else
    ng "-S 8 -s 4M → 종료 코드 $rc, 출력: $(head -1 "$TMP/opt.txt")"
fi
if grep -q "사용법" "$TMP/opt.txt"; then ng "  옵션끼리 맞지 않을 때는 사용법 전체를 출력하지 않아야 함"; else ok "  (사용법 전체는 출력하지 않음)"; fi

start_proxy opt8 18481 -S 8 -s 4M -o 512K || exit 1
if grep -q "캐시 락: 샤드 8개, 샤드마다 mutex (샤드 용량 512.0KB" "$LOGDIR/test5_opt8.log"; then
    ok "-S 8 -s 4M -o 512K → 실행, 시작 기록에 샤드 정보"
else
    ng "-S 8 시작 기록 없음"
fi
start_proxy opt1 18482 || exit 1
if grep -q "캐시 락: 전역 mutex 1개 (-S 1)" "$LOGDIR/test5_opt1.log"; then
    ok "옵션 없음 → 기본값 전역 mutex 1개"
else
    ng "-S 1 시작 기록 없음"
fi
start_proxy optoff 18483 -s 0 -S 8 || exit 1
expect "-s 0 -S 8 (캐시 끔) → 실행, /__stats의 샤드 수" "0 0 []" \
       "$(sget 18483 "str(d['cache']['shards']) + ' ' + str(d['config']['cache_shards']) + ' ' + str(d['cache']['shard_objects'])")"
stop_proc optoff

echo "[2] /__stats의 샤드·락 정보 (-S 8 프록시에 객체 40개 저장 후)"
for i in $(seq 0 39); do
    curl -s -o /dev/null -x http://127.0.0.1:18481 "$(printf 'http://%s/obj/%04d.bin' "$O" "$i")"
done
curl -s http://127.0.0.1:18481/__stats > "$TMP/s8.json"
schema=$(python3 - "$TMP/s8.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
c, L = d["cache"], d["locks"]
errs = []
if c["shards"] != 8 or d["config"]["cache_shards"] != 8: errs.append("shards")
if len(c["shard_objects"]) != 8 or len(c["shard_bytes"]) != 8: errs.append("배열 길이")
if sum(c["shard_objects"]) != c["objects"]: errs.append("shard_objects 합")
if sum(c["shard_bytes"]) != c["bytes"]: errs.append("shard_bytes 합")
if c["capacity"] != 4 * 1048576 or c["max_object"] != 524288: errs.append("capacity/max_object")
for k in ("cache", "stats"):
    if set(L[k]) != {"acquires", "contended", "wait_us"}: errs.append(f"locks.{k} 키")
if L["cache"]["acquires"] != 2 * 40: errs.append(f"캐시 락 획득 {L['cache']['acquires']} (기대 80)")
if L["stats"]["acquires"] != 40: errs.append(f"통계 락 획득 {L['stats']['acquires']} (기대 40)")
per = c["capacity"] // 8
if any(b > per and n > 1 for b, n in zip(c["shard_bytes"], c["shard_objects"])): errs.append("샤드 용량 초과")
print("; ".join(errs) or "ok")
EOF
)
expect "JSON: shards·shard_objects·shard_bytes·locks, 합계 일치, 락 획득 = 캐시 연산 수" ok "$schema"
echo "      샤드별 객체 수 $(jget "$TMP/s8.json" "d['cache']['shard_objects']"), 교체 $(jget "$TMP/s8.json" "d['cache']['evictions']")회" \
     "(용량 4MB 중 $(jget "$TMP/s8.json" "round(d['cache']['bytes'] / 1048576, 2)")MB 사용)"
c0=$(sget 18481 "d['locks']['cache']['acquires']")
for _ in 1 2 3 4 5; do curl -s -o /dev/null http://127.0.0.1:18481/__stats; done
expect "/__stats 조회는 락 획득 수에 세지 않음 (5번 조회 뒤)" "$c0" "$(sget 18481 "d['locks']['cache']['acquires']")"
stop_proc opt8
stop_proc opt1

echo "[3] 같은 요청열 3000개 (동시 1, 용량 64MB: 교체 없음) → -S 1과 -S 8의 결과가 같아야 함"
for s in 1 8; do
    start_proxy "eq$s" "1848$((3 + s / 8))" -S "$s" || exit 1      # -S 1 → 18483, -S 8 → 18484
done
for s in 1 8; do
    port="1848$((3 + s / 8))"
    origin_reset
    "${LG[@]}" --proxies "127.0.0.1:$port" -n 3000 -c 1 --json "$TMP/eq$s.json" > /dev/null
    echo "$? $(origin_total)" > "$TMP/eq$s.rc"
    sget "$port" "d['cache']['objects'], d['cache']['evictions'], d['locks']['cache']['acquires'], d['locks']['stats']['acquires'], d['locks']['cache']['contended']" > "$TMP/eq$s.st"
    sget "$port" "d['cache']['shard_objects']" > "$TMP/eq$s.sh"
done
pred=$(jget "$TMP/eq1.json" "d['predict_origin']['independent']")
for s in 1 8; do
    expect "-S $s: 부하 생성기 종료 코드, 원서버 요청 (예측 $pred)" "0 $pred" "$(cat "$TMP/eq$s.rc")"
done
f1=$(jget "$TMP/eq1.json" "d['result']['final']"); f8=$(jget "$TMP/eq8.json" "d['result']['final']")
expect "결과별 개수 (-S 8 = -S 1)" "$f1" "$f8"
st1=$(cat "$TMP/eq1.st"); st8=$(cat "$TMP/eq8.st")
expect "(객체 수, 교체, 캐시 락 획득, 통계 락 획득, 경합) (-S 8 = -S 1)" "$st1" "$st8"
expect "  (객체 = 예측, 교체 0, 캐시 락 = 요청 3000 × 2, 통계 락 = 3000, 경합 0)" "$pred 0 6000 3000 0" "$st1"
truth "-S 8: 객체 $pred개가 8개 샤드에 모두 퍼짐 $(cat "$TMP/eq8.sh")" "min($(cat "$TMP/eq8.sh")) > 0"
stop_proc eq1
stop_proc eq8

echo "[4] 같은 요청열, 용량 4MB (교체가 일어남): -S 1과 -S 8의 차이 (근사 LRU 관찰)"
for s in 1 8; do
    port="1848$((5 + s / 8))"                                       # -S 1 → 18485, -S 8 → 18486
    start_proxy "small$s" "$port" -S "$s" -s 4M -o 512K || exit 1
    origin_reset
    "${LG[@]}" --proxies "127.0.0.1:$port" -n 3000 -c 1 --json "$TMP/small$s.json" > /dev/null
    echo "$(jget "$TMP/small$s.json" "round(100 * d['result']['hit_ratio'], 1)") $(origin_total) $(sget "$port" "d['cache']['evictions']")" > "$TMP/small$s.r"
    sget "$port" "all(b <= d['cache']['capacity'] // d['cache']['shards'] or n <= 1 for b, n in zip(d['cache']['shard_bytes'], d['cache']['shard_objects']))" > "$TMP/small$s.cap"
    stop_proc "small$s"
done
read -r h1 o1 e1 < "$TMP/small1.r"; read -r h8 o8 e8 < "$TMP/small8.r"
echo "      히트율 -S 1 ${h1}% (원서버 요청 $o1, 교체 $e1회) / -S 8 ${h8}% (원서버 요청 $o8, 교체 $e8회)"
expect "두 설정 모두 샤드마다 용량 규칙을 지킴" "True True" "$(cat "$TMP/small1.cap") $(cat "$TMP/small8.cap")"
truth "교체가 실제로 일어남 (-S 1: $e1회, -S 8: $e8회)" "$e1 > 0 and $e8 > 0"

echo "[5] 동시 부하: -S 8 프록시에 요청 4000개, 동시 32"
start_proxy con8 18484 -S 8 || exit 1
origin_reset
"${LG[@]}" --proxies "127.0.0.1:18484" -n 4000 -c 32 --seed 7 --stats --json "$TMP/con.json" > "$TMP/con.txt"
rc=$?
expect "부하 생성기 종료 코드 (실패 없음 + 노드 통계 확인 일치)" 0 "$rc"
[ "$rc" -eq 0 ] || sed 's/^/      /' "$TMP/con.txt"
read -r req ca cc sa sc < <(sget 18484 "d['requests']['client'], d['locks']['cache']['acquires'], d['locks']['cache']['contended'], d['locks']['stats']['acquires'], d['locks']['stats']['contended']")
expect "캐시 락 획득 = 요청 수 × 2 (동시에 실행돼도 빠짐없이 셈)" "$((req * 2))" "$ca"
expect "통계 락 획득 = 요청 수" "$req" "$sa"
echo "      경합: 캐시 락 ${cc}회 / 통계 락 ${sc}회 (동시 32, Python 부하 생성기 기준)"
stop_proc con8

echo "[6] 클러스터 노드 3개를 -S 8로: 4단계 정확성이 그대로인가 (빈 캐시, 요청 3000개, 동시 1)"
CONF="$TMP/cluster.conf"
printf 'node1 127.0.0.1 18491\nnode2 127.0.0.1 18492\nnode3 127.0.0.1 18493\n' > "$CONF"
for n in node1 node2 node3; do
    "$BIN" -n "$n" -C "$CONF" -q -v 600 -r 3000 -S 8 >> "$LOGDIR/test5_c$n.log" 2>&1 &
    PID[c$n]=$!
done
for p in 18491 18492 18493; do wait_port "$p" || exit 1; done
origin_reset
CL="127.0.0.1:18491,127.0.0.1:18492,127.0.0.1:18493"
"${LG[@]}" --proxies "$CL" -n 3000 -c 1 --stats --json "$TMP/cl.json" > "$TMP/cl.txt"
rc=$?
expect "부하 생성기 종료 코드 (노드 통계 확인 포함)" 0 "$rc"
[ "$rc" -eq 0 ] || sed 's/^/      /' "$TMP/cl.txt"
pred=$(jget "$TMP/cl.json" "d['predict_origin']['cluster']")
expect "원서버 요청 수 = 예측값" "$pred" "$(origin_total)"
objs=0; for p in 18491 18492 18493; do objs=$((objs + $(sget $p "d['cache']['objects']"))); done
expect "세 노드 캐시 객체 수의 합 = 서로 다른 객체 수 (단일 사본)" "$pred" "$objs"
expect "세 노드 모두 샤드 8개" "8 8 8" "$(for p in 18491 18492 18493; do sget $p "d['cache']['shards']"; done | tr '\n' ' ' | sed 's/ $//')"
for n in node1 node2 node3; do stop_proc "c$n"; done

echo "[7] 정상 종료, 통계, 새니타이저"
if [[ "$RCS" =~ ^0+$ ]]; then
    ok "프록시 ${#RCS}개 모두 종료 코드 0"
else
    ng "종료 코드: $RCS (모두 0이어야 함)"
fi
if grep -q "락: 캐시(샤드 8개) | 획득" "$LOGDIR/test5_cnode1.log" && grep -q "락: 통계(mutex 1개) | 획득" "$LOGDIR/test5_cnode1.log"; then
    ok "종료 시 락 경합 요약 출력 (캐시·통계)"
else
    ng "종료 시 락 요약 없음"
fi
reports=$(cat "$LOGDIR"/test5_*.log | grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:')
expect "새니타이저 경고 수 (MODE=$MODE)" 0 "$reports"

echo
echo "결과: 통과 $pass개, 실패 $fail개   (로그: $LOGDIR/test5_*.log)"
[ "$fail" -eq 0 ]

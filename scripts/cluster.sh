#!/usr/bin/env bash
# 원서버와 클러스터 노드를 한 번에 실행·종료한다
#
# 사용법
#   ./scripts/cluster.sh start   [이름...]   실행 (이름을 안 주면 origin과 모든 노드)
#   ./scripts/cluster.sh stop    [이름...]   종료 (노드마다 통계 요약 출력)
#   ./scripts/cluster.sh restart [이름...]
#   ./scripts/cluster.sh status
#   이름: origin 또는 노드 목록 파일의 노드 이름 (node1, node2, node3)
#
# 환경 변수 (명령 앞에 붙여서 쓴다. 예: MODE=release ./scripts/cluster.sh start)
#   MODE=debug|release|tsan|asan   빌드 모드 (기본 debug)
#   CLUSTER=0                      노드 목록 없이 독립 프록시로 실행 (클러스터와 비교하는 실험용)
#   CONF=conf/cluster.conf         노드 목록 파일
#   PROXY_ARGS="-s 32M -q"         모든 노드에 덧붙일 프록시 옵션
#   ORIGIN_PORT=9000               원서버 포트
#   ORIGIN_DELAY=0                 원서버 응답 지연(초)
#
# 기록: ~/coop-data/logs/<이름>.log      실행 중인 프로세스 번호: ~/coop-data/run/<이름>.pid
set -u
cd "$(dirname "$0")/.." || exit 1

MODE=${MODE:-debug}
CLUSTER=${CLUSTER:-1}
CONF=${CONF:-conf/cluster.conf}
PROXY_ARGS=${PROXY_ARGS:-}
ORIGIN_PORT=${ORIGIN_PORT:-9000}
ORIGIN_DELAY=${ORIGIN_DELAY:-0}
DATA=${COOP_DATA:-$HOME/coop-data}
WWW="$DATA/www"
LOGDIR="$DATA/logs"
RUNDIR="$DATA/run"
mkdir -p "$LOGDIR" "$RUNDIR"

# 노드 목록 읽기: 주석(#)과 빈 줄을 빼고 "이름 호스트 포트"
declare -A PORT
NODES=()
while read -r name _host port _rest; do
    NODES+=("$name")
    PORT[$name]=$port
done < <(sed -e 's/#.*//' "$CONF" | awk 'NF >= 3')

pid_of()   { cat "$RUNDIR/$1.pid" 2>/dev/null; }
is_alive() { local p; p=$(pid_of "$1"); [ -n "$p" ] && kill -0 "$p" 2>/dev/null; }
port_of()  { if [ "$1" = origin ]; then echo "$ORIGIN_PORT"; else echo "${PORT[$1]}"; fi; }

wait_port() {
    for _ in $(seq 1 50); do
        nc -z 127.0.0.1 "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

start_one() {
    local name=$1 port pid
    if [ "$name" != origin ] && [ -z "${PORT[$name]+x}" ]; then
        echo "  ✗ $name: $CONF 에 없는 이름입니다"
        return 1
    fi
    if is_alive "$name"; then
        echo "  - $name: 이미 실행 중 (pid $(pid_of "$name"))"
        return 0
    fi
    port=$(port_of "$name")
    if nc -z 127.0.0.1 "$port" 2>/dev/null; then
        echo "  ✗ $name: 포트 $port 를 다른 프로그램이 쓰고 있습니다 (ss -ltnp | grep $port 로 확인)"
        return 1
    fi

    # nohup: 터미널을 닫아도 계속 실행. < /dev/null: 표준 입력을 쓰지 않음
    if [ "$name" = origin ]; then
        nohup python3 tools/origin.py --port "$port" --dir "$WWW" --delay "$ORIGIN_DELAY" --quiet \
            < /dev/null > "$LOGDIR/origin.log" 2>&1 &
    elif [ "$CLUSTER" = 1 ]; then
        # shellcheck disable=SC2086  (PROXY_ARGS는 일부러 단어로 나눈다)
        nohup "build/$MODE/proxy" -n "$name" -C "$CONF" $PROXY_ARGS \
            < /dev/null > "$LOGDIR/$name.log" 2>&1 &
    else
        # shellcheck disable=SC2086
        nohup "build/$MODE/proxy" -n "$name" -p "$port" $PROXY_ARGS \
            < /dev/null > "$LOGDIR/$name.log" 2>&1 &
    fi
    pid=$!
    echo "$pid" > "$RUNDIR/$name.pid"

    if wait_port "$port"; then
        echo "  ✓ $name  127.0.0.1:$port  (pid $pid)"
    else
        echo "  ✗ $name: 시작하지 못했습니다 → $LOGDIR/$name.log"
        tail -3 "$LOGDIR/$name.log" | sed 's/^/      /'
        rm -f "$RUNDIR/$name.pid"
        return 1
    fi
}

stop_one() {
    local name=$1 pid
    if ! is_alive "$name"; then
        echo "  - $name: 실행 중이 아님"
        rm -f "$RUNDIR/$name.pid"
        return 0
    fi
    pid=$(pid_of "$name")
    # SIGTERM: 프록시는 진행 중인 요청을 마치고 통계를 남긴 뒤 종료한다.
    # (SIGINT는 스크립트가 띄운 Python에는 무시로 설정되어 있어 쓰지 않는다)
    kill -TERM "$pid"
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid"
        echo "  ! $name: 10초 안에 끝나지 않아 강제 종료했습니다"
    fi
    rm -f "$RUNDIR/$name.pid"
    echo "  ✓ $name 종료"
    if [ "$name" != origin ]; then
        grep -E '\] (통계|클러스터|캐시):' "$LOGDIR/$name.log" | tail -4 | sed 's/^.*\] /      /'
    fi
}

status_all() {
    local name port
    for name in origin "${NODES[@]}"; do
        port=$(port_of "$name")
        if is_alive "$name"; then
            echo "  ● $name  127.0.0.1:$port  실행 중 (pid $(pid_of "$name"))"
        else
            echo "  ○ $name  127.0.0.1:$port  중지됨"
        fi
    done
}

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

cmd=${1:-}
[ $# -gt 0 ] && shift
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(origin "${NODES[@]}")

do_start() {
    make -s MODE="$MODE" || exit 1
    if [ ! -f "$WWW/big.bin" ]; then
        echo "테스트 파일 생성 → $WWW"
        python3 tools/gen_files.py --dir "$WWW" > /dev/null || exit 1
    fi
    if [ "$CLUSTER" = 1 ]; then
        echo "시작 (MODE=$MODE, 클러스터: $CONF)"
    else
        echo "시작 (MODE=$MODE, 독립 프록시)"
    fi
    local rc=0 n list=""
    for n in "${targets[@]}"; do start_one "$n" || rc=1; done
    # 관제 화면 주소 (독립 프록시는 서로를 모르므로 노드 목록을 주소에 붙인다)
    for n in "${NODES[@]}"; do list="$list${list:+,}127.0.0.1:${PORT[$n]}"; done
    if [ "$CLUSTER" = 1 ]; then
        echo "관제 화면: http://localhost:${PORT[${NODES[0]}]}/__dashboard"
    else
        echo "관제 화면: http://localhost:${PORT[${NODES[0]}]}/__dashboard?nodes=$list"
    fi
    return $rc
}

do_stop() {
    echo "종료"
    local i
    for (( i=${#targets[@]}-1; i>=0; i-- )); do     # 노드부터 끄고 원서버는 마지막에
        stop_one "${targets[$i]}"
    done
}

case "$cmd" in
    start)   do_start ;;
    stop)    do_stop ;;
    restart) do_stop; do_start ;;
    status)  status_all ;;
    *)       usage ;;
esac

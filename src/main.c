/*
 * main.c — 프로그램 시작점
 *
 *  - 실행 인자 해석, 차단 목록 읽기, 캐시 생성
 *  - 시그널 준비: SIGPIPE 무시, Ctrl+C(SIGINT)·SIGTERM은 전용 스레드가 받아 정상 종료
 *  - 리스닝 소켓 → accept → 연결마다 작업 스레드 생성
 *  - 종료: 진행 중인 연결이 끝나길 기다린 뒤 통계 출력, 캐시 해제
 */
#include "cache.h"
#include "filter.h"
#include "log.h"
#include "net.h"
#include "proxy.h"
#include "router.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int        g_listen_fd = -1;
static atomic_int g_stopping;       /* 종료 중이면 1. 두 스레드가 함께 보므로 원자 변수 */

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [옵션]\n"
            "  -p PORT   대기할 포트 (기본 8080)\n"
            "  -n NAME   노드 이름 (기본 node1)\n"
            "  -c MS     원서버 연결 제한 시간, 밀리초 (기본 3000)\n"
            "  -t MS     송수신 제한 시간, 밀리초 (기본 5000)\n"
            "  -s SIZE   캐시 용량, 예: 64M, 512K (기본 64M, 0이면 캐시 끔)\n"
            "  -o SIZE   캐시에 저장할 객체 하나의 상한 (기본 1M)\n"
            "  -S N      캐시 샤드 수 1~64 (기본 1 = 전역 mutex 하나, 2 이상 = 샤드마다 mutex)\n"
            "            용량은 샤드마다 1/N씩 나누며, 샤드 하나의 용량이 객체 상한 이상이어야 한다\n"
            "  -v SEC    검증 주기, 초: 저장 후 이 시간이 지나면 원서버에 확인 (기본 60)\n"
            "  -b FILE   차단 목록 파일 (예: conf/blocklist.txt)\n"
            "  -C FILE   노드 목록 파일 (예: conf/cluster.conf). 주면 클러스터 모드로 동작\n"
            "            -n의 이름이 목록에 있어야 하고, -p를 생략하면 목록의 포트를 쓴다\n"
            "  -r MS     연결에 실패한 노드를 건너뛰는 시간, 밀리초 (기본 5000)\n"
            "  -q        요청 기록을 출력하지 않음 (부하 실험용)\n"
            "  -h        도움말\n",
            prog);
}

/* "64M", "512K", "1048576" 같은 크기 문자열을 바이트로 바꾼다. 실패하면 -1 */
static long long parse_size(const char *s)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || v < 0)
        return -1;
    switch (*end) {
    case '\0':           break;
    case 'k': case 'K':  v *= 1024LL;               end++; break;
    case 'm': case 'M':  v *= 1024LL * 1024;        end++; break;
    case 'g': case 'G':  v *= 1024LL * 1024 * 1024; end++; break;
    default:             return -1;
    }
    return *end == '\0' ? v : -1;
}

/* 관제 화면 파일(web/dashboard.html)을 찾아 절대 경로를 out에 넣는다. 못 찾으면 빈 문자열.
 *   ① 현재 폴더 기준: 프로젝트 폴더에서 실행했을 때 (cluster.sh, VS Code 디버거)
 *   ② 실행 파일 기준: build/<모드>/proxy 에서 두 단계 위 폴더 → 어디서 실행해도 찾는다 */
static void find_dashboard(char *out, size_t cap)
{
    out[0] = '\0';
    char *rp = realpath("web/dashboard.html", NULL);
    if (rp != NULL) {
        if (access(rp, R_OK) == 0 && strlen(rp) < cap)
            memcpy(out, rp, strlen(rp) + 1);
        free(rp);
        if (out[0] != '\0')
            return;
    }

    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);   /* 실행 파일의 실제 경로 */
    if (n <= 0)
        return;
    exe[n] = '\0';
    for (int up = 0; up < 3; up++) {
        char *slash = strrchr(exe, '/');
        if (slash == NULL)
            return;
        *slash = '\0';
    }
    const char *suffix = "/web/dashboard.html";
    if (strlen(exe) + strlen(suffix) >= cap)
        return;
    memcpy(out, exe, strlen(exe));
    memcpy(out + strlen(exe), suffix, strlen(suffix) + 1);
    if (access(out, R_OK) != 0)
        out[0] = '\0';
}

/* 바이트 수를 읽기 좋게 */
static void human_size(size_t b, char *out, size_t cap)
{
    if (b >= 1024 * 1024)
        snprintf(out, cap, "%.1fMB", (double)b / (1024.0 * 1024.0));
    else if (b >= 1024)
        snprintf(out, cap, "%.1fKB", (double)b / 1024.0);
    else
        snprintf(out, cap, "%zuB", b);
}

/* 문자열을 범위 안의 정수로 바꾼다. 실패하면 -1 */
static int parse_int(const char *s, int min, int max)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    return (int)v;
}

static int parse_args(int argc, char *argv[], struct proxy_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->port               = 8080;
    cfg->connect_timeout_ms = 3000;
    cfg->io_timeout_ms      = 5000;
    cfg->cache_capacity     = 64 * 1024 * 1024;
    cfg->max_object         = 1024 * 1024;
    cfg->cache_shards       = 1;
    cfg->validate_sec       = 60;
    cfg->peer_retry_ms      = 5000;
    snprintf(cfg->node, sizeof cfg->node, "node1");

    long long size;
    int opt;
    while ((opt = getopt(argc, argv, "p:n:c:t:s:o:S:v:b:C:r:qh")) != -1) {
        switch (opt) {
        case 'p':
            if ((cfg->port = parse_int(optarg, 1, 65535)) < 0) {
                fprintf(stderr, "잘못된 포트: %s\n", optarg);
                return -1;
            }
            cfg->port_given = 1;
            break;
        case 'n':
            snprintf(cfg->node, sizeof cfg->node, "%s", optarg);
            break;
        case 'c':
            if ((cfg->connect_timeout_ms = parse_int(optarg, 1, 600000)) < 0) {
                fprintf(stderr, "잘못된 연결 제한 시간: %s\n", optarg);
                return -1;
            }
            break;
        case 't':
            if ((cfg->io_timeout_ms = parse_int(optarg, 1, 600000)) < 0) {
                fprintf(stderr, "잘못된 송수신 제한 시간: %s\n", optarg);
                return -1;
            }
            break;
        case 's':
            if ((size = parse_size(optarg)) < 0) {
                fprintf(stderr, "잘못된 캐시 용량: %s\n", optarg);
                return -1;
            }
            cfg->cache_capacity = (size_t)size;
            break;
        case 'o':
            if ((size = parse_size(optarg)) <= 0) {
                fprintf(stderr, "잘못된 객체 상한: %s\n", optarg);
                return -1;
            }
            cfg->max_object = (size_t)size;
            break;
        case 'S':
            if ((cfg->cache_shards = parse_int(optarg, 1, CACHE_MAX_SHARDS)) < 0) {
                fprintf(stderr, "잘못된 샤드 수: %s (1~%d)\n", optarg, CACHE_MAX_SHARDS);
                return -1;
            }
            break;
        case 'v':
            if ((cfg->validate_sec = parse_int(optarg, 0, 86400)) < 0) {
                fprintf(stderr, "잘못된 검증 주기: %s\n", optarg);
                return -1;
            }
            break;
        case 'b':
            snprintf(cfg->blocklist, sizeof cfg->blocklist, "%s", optarg);
            break;
        case 'C':
            snprintf(cfg->cluster_conf, sizeof cfg->cluster_conf, "%s", optarg);
            break;
        case 'r':
            if ((cfg->peer_retry_ms = parse_int(optarg, 100, 600000)) < 0) {
                fprintf(stderr, "잘못된 재시도 간격: %s (100~600000)\n", optarg);
                return -1;
            }
            break;
        case 'q':
            cfg->quiet = 1;
            break;
        default:
            return -1;
        }
    }
    if (cfg->cache_capacity == 0)
        return 0;                                   /* 캐시 끔: 용량·샤드 검사 없음 */

    size_t per = cfg->cache_capacity / (size_t)cfg->cache_shards;    /* 샤드 하나의 용량 */
    char cap[32], obj[32], per_s[32], need[32];
    human_size(cfg->cache_capacity, cap, sizeof cap);
    human_size(cfg->max_object, obj, sizeof obj);
    human_size(per, per_s, sizeof per_s);

    if (per == 0) {
        fprintf(stderr, "캐시 용량(%s)이 샤드 수(%d)보다 작습니다.\n", cap, cfg->cache_shards);
        return -2;
    }
    if (cfg->cache_shards == 1) {
        /* 전역 mutex: 객체 하나가 캐시 전체보다 클 수는 없으므로 상한을 용량으로 줄인다.
         * (2단계부터의 동작. 어차피 저장할 수 없는 크기이므로 의미가 바뀌지 않는다) */
        if (cfg->max_object > cfg->cache_capacity)
            cfg->max_object = cfg->cache_capacity;
    } else if (cfg->max_object > per) {
        /* 샤딩: 여기서 상한을 조용히 줄이면, 캐시 전체에는 들어가는 객체가 샤드 수 때문에
         * 저장되지 않는다. 그러면 샤드 수에 따라 "저장되는 객체"가 달라져서
         * 락 전략 비교가 공정하지 않다. 그래서 줄이지 않고 실행을 거부한다 */
        human_size(cfg->max_object * (size_t)cfg->cache_shards, need, sizeof need);
        fprintf(stderr,
                "샤드 하나의 용량(%s = %s ÷ %d)이 객체 상한(%s)보다 작습니다.\n"
                "샤드를 나누면 객체 하나는 샤드 하나에 들어가야 합니다.\n"
                "-s를 %s 이상으로 늘리거나, -o를 %s 이하로 주세요.\n",
                per_s, cap, cfg->cache_shards, obj, need, per_s);
        return -2;
    }
    return 0;
}

/* 시그널 전용 스레드.
 * 멀티스레드 프로그램에서 시그널 핸들러는 "아무 스레드"에서나 실행될 수 있어 다루기
 * 까다롭다. 그래서 모든 스레드에서 SIGINT·SIGTERM을 막아 두고, 이 스레드만
 * sigwait()로 기다렸다가 평범한 코드로 처리한다 */
static void *signal_thread(void *arg)
{
    const sigset_t *set = arg;
    int sig;
    if (sigwait(set, &sig) == 0) {
        log_info("%s 시그널을 받았습니다. 종료합니다.", sig == SIGINT ? "SIGINT" : "SIGTERM");
        atomic_store(&g_stopping, 1);
        /* 메인 스레드는 accept()에서 멈춰 있다. 리스닝 소켓을 shutdown하면
         * accept()가 오류로 돌아오면서 메인 스레드가 깨어난다 */
        shutdown(g_listen_fd, SHUT_RDWR);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    struct proxy_config cfg;
    int pr = parse_args(argc, argv, &cfg);
    if (pr == -1)
        usage(argv[0]);             /* 옵션 형식 오류: 사용법을 보여 준다 */
    if (pr < 0)
        return 2;                   /* -2: 옵션끼리 맞지 않음 (이유는 이미 출력함) */

    setvbuf(stdout, NULL, _IOLBF, 0);   /* 기록을 파일로 돌려도 한 줄씩 바로 쓰이게 */
    log_init(cfg.node, cfg.quiet);

    if (cfg.blocklist[0] != '\0' && filter_load(cfg.blocklist) < 0) {
        log_info("차단 목록 파일을 읽을 수 없습니다: %s", cfg.blocklist);
        return 1;
    }
    /* 클러스터 모드: 노드 목록을 읽고, 이 노드의 포트를 목록과 맞춘다 */
    if (cfg.cluster_conf[0] != '\0') {
        char err[256];
        if (router_load(cfg.cluster_conf, cfg.node, err, sizeof err) < 0) {
            log_info("%s", err);
            return 1;
        }
        const struct node *me = router_node(router_self());
        if (!cfg.port_given) {
            cfg.port = me->port;
        } else if (cfg.port != me->port) {
            log_info("-p %d가 %s에 적힌 %s의 포트 %d와 다릅니다.",
                     cfg.port, cfg.cluster_conf, me->name, me->port);
            return 1;
        }
    }

    find_dashboard(cfg.dashboard_path, sizeof cfg.dashboard_path);

    if (proxy_init(&cfg) < 0) {
        log_info("캐시를 만들 수 없습니다.");
        return 1;
    }

    /* 끊긴 연결에 쓰면 SIGPIPE가 와서 프로세스가 죽는다 → 무시하고 EPIPE 오류로 처리 */
    signal(SIGPIPE, SIG_IGN);

    /* SIGINT·SIGTERM의 처리 방식을 기본값으로 되돌린다.
     * 셸 스크립트에서 '명령 &'로 띄운 프로그램은 SIGINT가 "무시"로 설정된 채 시작된다
     * (작업 제어가 없는 셸의 규칙). 리눅스는 막아 둔(blocked) 시그널을 무시 설정이어도
     * 보관해 두지만, 이것은 표준이 보장하지 않으므로 명시적으로 되돌린다 */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    /* SIGINT·SIGTERM 차단: 스레드를 만들기 "전에" 막아야 모든 스레드가 이 설정을 물려받는다 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    g_listen_fd = net_listen(cfg.port, 128);
    if (g_listen_fd < 0) {
        log_info("포트 %d에서 대기할 수 없습니다: %s", cfg.port, strerror(errno));
        return 1;
    }

    pthread_t sig_tid;
    if (pthread_create(&sig_tid, NULL, signal_thread, &set) != 0) {
        log_info("시그널 스레드를 만들 수 없습니다.");
        return 1;
    }

    /* 작업 스레드는 detached로 만든다: 끝나면 자원을 스스로 반납하고 join할 필요가 없다 */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    log_info("포트 %d에서 대기 중 (연결 제한 %dms, 송수신 제한 %dms)%s",
             cfg.port, cfg.connect_timeout_ms, cfg.io_timeout_ms,
             cfg.quiet ? ", 요청 기록 끔" : "");
    if (cfg.cache_capacity > 0) {
        char cap[32], obj[32];
        human_size(cfg.cache_capacity, cap, sizeof cap);
        human_size(cfg.max_object, obj, sizeof obj);
        log_info("캐시 용량 %s, 객체 상한 %s, 검증 주기 %d초", cap, obj, cfg.validate_sec);
        if (cfg.cache_shards == 1) {
            log_info("캐시 락: 전역 mutex 1개 (-S 1)");
        } else {
            char per[32];
            human_size(cfg.cache_capacity / (size_t)cfg.cache_shards, per, sizeof per);
            log_info("캐시 락: 샤드 %d개, 샤드마다 mutex (샤드 용량 %s, 교체는 샤드 안에서)",
                     cfg.cache_shards, per);
        }
    } else {
        log_info("캐시 끔 (모든 요청을 원서버로 중계)");
    }
    if (filter_count() > 0)
        log_info("차단 목록 %zu개 (%s)", filter_count(), cfg.blocklist);
    if (router_enabled()) {
        char list[512] = "";
        size_t len = 0;
        for (int i = 0; i < router_count() && len < sizeof list; i++) {
            const struct node *nd = router_node(i);
            int w = snprintf(list + len, sizeof list - len, "%s%s(%s:%d%s)",
                             i ? ", " : "", nd->name, nd->host, nd->port,
                             i == router_self() ? ", 이 노드" : "");
            if (w < 0)
                break;
            len += (size_t)w;
        }
        log_info("클러스터 노드 %d개: %s", router_count(), list);
        log_info("담당 노드 연결 실패 시 2순위로 대체, 실패한 노드는 %dms 동안 건너뜀",
                 cfg.peer_retry_ms);
    } else {
        log_info("독립 프록시 (클러스터 설정 없음)");
    }
    if (cfg.dashboard_path[0] != '\0')
        log_info("관제 화면: http://127.0.0.1:%d/__dashboard  (통계 JSON: /__stats)", cfg.port);
    else
        log_info("관제 화면 파일(web/dashboard.html)을 찾지 못했습니다. /__stats만 제공합니다.");

    unsigned long accepted = 0;
    for (;;) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof cli;
        int cfd = accept(g_listen_fd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) {
            if (atomic_load(&g_stopping))
                break;
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                /* 열 수 있는 파일 디스크립터가 바닥남 → 잠시 쉬었다가 다시 시도 */
                log_info("파일 디스크립터 부족: %s", strerror(errno));
                usleep(100 * 1000);
                continue;
            }
            log_info("accept 실패: %s", strerror(errno));
            continue;
        }

        /* 연결 정보는 스레드마다 힙에 따로 만든다 (지역 변수 주소를 넘기면
         * 스레드가 읽기 전에 다음 accept가 덮어쓸 수 있다) */
        struct client_conn *conn = malloc(sizeof *conn);
        if (conn == NULL) {
            close(cfd);
            continue;
        }
        conn->fd = cfd;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof ip);
        snprintf(conn->addr, sizeof conn->addr, "%s:%u", ip, ntohs(cli.sin_port));

        /* 진행 중인 연결 수를 "스레드를 만들기 전에" 늘린다. 스레드 안에서 늘리면,
         * 스레드가 실행되기 전에 종료 절차가 "연결 0개"로 잘못 판단할 수 있다 */
        proxy_conn_opened();
        pthread_t tid;
        int err = pthread_create(&tid, &attr, proxy_handle_connection, conn);
        if (err != 0) {
            log_info("작업 스레드를 만들 수 없습니다: %s", strerror(err));
            proxy_conn_aborted();
            close(cfd);
            free(conn);
            continue;
        }
        accepted++;
    }

    pthread_attr_destroy(&attr);
    close(g_listen_fd);
    pthread_join(sig_tid, NULL);

    /* 진행 중인 연결이 끝나길 최대 5초 기다린다. 작업 스레드가 아직 캐시를 쓰는 중에
     * 캐시를 해제하면 해제된 메모리를 읽게 되기 때문이다 */
    for (int i = 0; i < 500 && proxy_active_conns() > 0; i++)
        usleep(10 * 1000);

    log_info("종료: 연결 %lu개를 처리했습니다.", accepted);
    proxy_report();
    if (proxy_active_conns() == 0)
        proxy_cleanup();
    else
        log_info("끝나지 않은 연결 %d개가 있어 캐시를 해제하지 않고 종료합니다.", proxy_active_conns());
    filter_free();
    return 0;
}

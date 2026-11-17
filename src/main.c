/*
 * main.c — 프로그램 시작점
 *
 *  - 실행 인자 해석
 *  - 시그널 준비: SIGPIPE 무시, Ctrl+C(SIGINT)·SIGTERM은 전용 스레드가 받아 정상 종료
 *  - 리스닝 소켓 → accept → 연결마다 작업 스레드 생성
 */
#include "log.h"
#include "net.h"
#include "proxy.h"

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
            "  -q        요청 기록을 출력하지 않음 (부하 실험용)\n"
            "  -h        도움말\n",
            prog);
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
    snprintf(cfg->node, sizeof cfg->node, "node1");

    int opt;
    while ((opt = getopt(argc, argv, "p:n:c:t:qh")) != -1) {
        switch (opt) {
        case 'p':
            if ((cfg->port = parse_int(optarg, 1, 65535)) < 0) {
                fprintf(stderr, "잘못된 포트: %s\n", optarg);
                return -1;
            }
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
        case 'q':
            cfg->quiet = 1;
            break;
        default:
            return -1;
        }
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
    if (parse_args(argc, argv, &cfg) < 0) {
        usage(argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);   /* 기록을 파일로 돌려도 한 줄씩 바로 쓰이게 */
    log_init(cfg.node, cfg.quiet);
    proxy_init(&cfg);

    /* 끊긴 연결에 쓰면 SIGPIPE가 와서 프로세스가 죽는다 → 무시하고 EPIPE 오류로 처리 */
    signal(SIGPIPE, SIG_IGN);

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

        pthread_t tid;
        int err = pthread_create(&tid, &attr, proxy_handle_connection, conn);
        if (err != 0) {
            log_info("작업 스레드를 만들 수 없습니다: %s", strerror(err));
            close(cfd);
            free(conn);
            continue;
        }
        accepted++;
    }

    pthread_attr_destroy(&attr);
    close(g_listen_fd);
    pthread_join(sig_tid, NULL);
    log_info("종료: 연결 %lu개를 처리했습니다.", accepted);
    return 0;
}

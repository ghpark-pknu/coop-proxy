/*
 * hello_server.c — 개발환경 검증용 최소 HTTP 서버
 *
 * 이 파일 하나로 다음이 모두 동작하는지 확인한다.
 *   - 소켓 API: socket → bind → listen → accept → read/write → close
 *   - pthread: 연결마다 작업 스레드 1개 (프록시와 같은 구조)
 *   - gcc, make, gdb, AddressSanitizer, ThreadSanitizer
 *   - curl, ab(부하 도구), Windows 브라우저 → WSL 접속
 *
 * 사용법: ./hello_server [포트] [--race]
 *   --race : 요청 카운터를 락 없이 증가시켜 일부러 데이터 경쟁을 만든다.
 *            MODE=tsan 빌드에서 ThreadSanitizer가 잡아내는지 확인하는 용도.
 */
#define _GNU_SOURCE /* gettid() 선언을 쓰기 위해 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned long g_count;                         /* 처리한 요청 수 (공유 변수) */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_race;                                    /* 1이면 락 없이 증가 */

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

/* write()는 요청한 길이보다 적게 쓰고 돌아올 수 있다(부분 쓰기).
 * 그래서 전부 보낼 때까지 반복한다. 프록시에서도 그대로 쓰게 될 패턴. */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;   /* 시그널로 중단되면 다시 시도 */
            return -1;                      /* EPIPE 등: 상대가 연결을 끊음 */
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static void *handle_client(void *arg)
{
    int cfd = *(int *)arg;
    free(arg);

    char req[4096];
    ssize_t n = read(cfd, req, sizeof req - 1);   /* 검증용이라 한 번만 읽는다 */
    if (n > 0) {
        req[n] = '\0';
        char *eol = strstr(req, "\r\n");          /* 요청 라인만 남긴다 */
        if (eol) *eol = '\0';

        unsigned long no;
        if (g_race) {
            no = ++g_count;                       /* 데이터 경쟁! (일부러) */
        } else {
            pthread_mutex_lock(&g_lock);
            no = ++g_count;
            pthread_mutex_unlock(&g_lock);
        }

        /* 부하 테스트 때 화면이 넘치지 않게 처음 5개와 1000개마다만 출력 */
        if (no <= 5 || no % 1000 == 0)
            printf("[#%lu] tid=%d  %s\n", no, (int)gettid(), req);

        /* 본문 길이는 항상 같게 두고(ab는 길이가 달라지면 실패로 셈),
         * 요청 번호와 스레드 ID는 헤더로 보낸다. curl -i로 확인할 수 있다. */
        const char *body = "hello from WSL\n";
        char resp[512];
        int rlen = snprintf(resp, sizeof resp,
                            "HTTP/1.0 200 OK\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: %zu\r\n"
                            "X-Request-No: %lu\r\n"
                            "X-Thread-Id: %d\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "%s",
                            strlen(body), no, (int)gettid(), body);
        write_all(cfd, resp, (size_t)rlen);
    }
    close(cfd);
    return NULL;
}

int main(int argc, char *argv[])
{
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--race") == 0)
            g_race = 1;
        else
            port = atoi(argv[i]);
    }

    setvbuf(stdout, NULL, _IOLBF, 0);   /* 출력을 리다이렉트해도 줄 단위로 바로 보이게 */
    signal(SIGPIPE, SIG_IGN);           /* 끊긴 연결에 write해도 프로세스가 죽지 않게 */

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) die("socket");

    /* 서버를 껐다 바로 켤 때 TIME_WAIT 때문에 bind가 실패하는 것을 막는다 */
    int on = 1;
    if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0)
        die("setsockopt");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 모든 인터페이스에서 받기 */
    addr.sin_port        = htons((unsigned short)port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) die("bind");
    if (listen(lfd, 128) < 0) die("listen");

    printf("hello_server: 포트 %d 에서 대기 중 (race=%s). 종료는 Ctrl+C\n",
           port, g_race ? "on" : "off");

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof cli;
        int cfd = accept(lfd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) {
            if (errno != EINTR) perror("accept");
            continue;
        }

        /* &cfd를 그대로 넘기면 다음 accept가 값을 덮어쓸 수 있다(경쟁 조건).
         * 그래서 연결마다 힙에 따로 복사해서 넘기고, 스레드가 free한다. */
        int *arg = malloc(sizeof *arg);
        if (arg == NULL) {
            close(cfd);
            continue;
        }
        *arg = cfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, arg) != 0) {
            perror("pthread_create");
            free(arg);
            close(cfd);
            continue;
        }
        pthread_detach(tid);   /* join하지 않는 스레드: 끝나면 자원을 스스로 반납 */
    }
}

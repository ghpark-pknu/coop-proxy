/*
 * net.c — 소켓 공통 기능 구현
 */
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* errno를 보존한 채 fd를 닫는다 (close가 errno를 덮어쓸 수 있으므로) */
static void close_keep_errno(int fd)
{
    int saved = errno;
    close(fd);
    errno = saved;
}

int net_listen(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    /* 프록시를 껐다가 바로 다시 켤 때, 이전 연결의 TIME_WAIT 상태 때문에
     * bind가 "Address already in use"로 실패하는 것을 막는다 */
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0)
        goto fail;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);       /* 모든 네트워크 인터페이스 */
    addr.sin_port        = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        goto fail;
    if (listen(fd, backlog) < 0)
        goto fail;
    return fd;

fail:
    close_keep_errno(fd);
    return -1;
}

/* 주소 하나에 대해 제한 시간이 있는 연결을 시도한다.
 *
 * 블로킹 connect()는 상대가 응답하지 않으면 커널 기본값(리눅스에서 약 2분)이
 * 지날 때까지 돌아오지 않는다. 그래서 다음 순서로 시간을 직접 제한한다.
 *   1) 소켓을 논블로킹으로 만들고 connect() → 곧바로 EINPROGRESS로 돌아옴
 *   2) poll()로 "쓰기 가능"해질 때까지 최대 timeout_ms 기다림
 *   3) 깨어나면 SO_ERROR로 연결 성공/실패를 확인
 *   4) 성공하면 다시 블로킹 모드로 되돌림 (이후 코드를 단순하게 쓰기 위해) */
static int connect_one(const struct addrinfo *ai, int timeout_ms, int *timed_out)
{
    int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd < 0)
        return -1;

    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        goto connected;                 /* 로컬 연결은 즉시 성공하기도 한다 */
    if (errno != EINPROGRESS)
        goto fail;                      /* 예: 즉시 거부(ECONNREFUSED) */

    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {                      /* 제한 시간 초과 */
        *timed_out = 1;
        errno = ETIMEDOUT;
        goto fail;
    }
    if (rc < 0)
        goto fail;

    /* poll이 깨어났다는 것은 "연결 시도가 끝났다"는 뜻일 뿐, 성공은 아닐 수 있다 */
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        goto fail;
    if (err != 0) {
        errno = err;
        goto fail;
    }

connected:;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
        goto fail;
    return fd;

fail:
    close_keep_errno(fd);
    return -1;
}

int net_connect(const char *host, int port, int timeout_ms)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);

    /* getaddrinfo: 호스트 이름 → 주소 목록 (IPv4·IPv6 모두 가능).
     * "localhost"는 ::1(IPv6)과 127.0.0.1(IPv4) 두 개가 나올 수 있으므로
     * 목록을 차례로 시도한다 */
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return NET_ERR_RESOLVE;

    int result = NET_ERR_CONNECT;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int timed_out = 0;
        int fd = connect_one(ai, timeout_ms, &timed_out);
        if (fd >= 0) {
            result = fd;
            break;
        }
        if (timed_out)
            result = NET_ERR_TIMEOUT;
    }
    freeaddrinfo(res);
    return result;
}

int net_set_timeouts(int fd, int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0)
        return -1;
    return 0;
}

int net_write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        /* MSG_NOSIGNAL: 상대가 이미 연결을 끊었을 때 SIGPIPE 대신 EPIPE 오류만 받는다 */
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;      /* EPIPE, ECONNRESET, EAGAIN(송신 제한 시간 초과) 등 */
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

long long net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

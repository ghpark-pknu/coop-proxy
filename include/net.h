/*
 * net.h — 소켓 공통 기능
 *
 * 프록시는 클라이언트 쪽으로는 서버(listen/accept), 원서버 쪽으로는
 * 클라이언트(connect)로 동작한다. 두 역할에 공통으로 필요한 소켓 작업을
 * 이 모듈에 모아 둔다.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>

/* net_connect()의 실패 원인 (음수로 반환) */
enum net_error {
    NET_ERR_RESOLVE = -1,   /* 호스트 이름을 IP 주소로 바꾸지 못함 (DNS 실패) */
    NET_ERR_CONNECT = -2,   /* 연결 거부, 경로 없음 등 */
    NET_ERR_TIMEOUT = -3    /* 제한 시간 안에 연결되지 않음 */
};

/* 0.0.0.0:port 에서 연결을 기다리는 리스닝 소켓을 만든다.
 * 성공하면 소켓 fd, 실패하면 -1 (errno에 원인) */
int net_listen(int port, int backlog);

/* host:port 로 TCP 연결을 맺는다. timeout_ms 안에 연결되지 않으면 포기한다.
 * 성공하면 연결된 소켓 fd(블로킹 모드), 실패하면 enum net_error 값(음수) */
int net_connect(const char *host, int port, int timeout_ms);

/* 소켓의 수신·송신 제한 시간을 설정한다 (SO_RCVTIMEO, SO_SNDTIMEO).
 * 이후 recv/send가 이 시간 동안 진척이 없으면 -1을 반환하고 errno는 EAGAIN */
int net_set_timeouts(int fd, int timeout_ms);

/* len 바이트를 모두 보낼 때까지 반복한다. 성공 0, 실패 -1 */
int net_write_all(int fd, const void *buf, size_t len);

/* 떨어져 있는 여러 조각을 시스템 호출 한 번에 이어서 보낸다 (모으기 쓰기, gather write).
 * 조각을 send()로 따로 보내면 작은 TCP 세그먼트가 여러 개 생겨 Nagle 알고리즘
 * 때문에 지연될 수 있다. iov 배열은 진행 상황에 따라 수정된다. 성공 0, 실패 -1 */
struct iovec;
int net_writev_all(int fd, struct iovec *iov, int iovcnt);

/* 경과 시간 측정용 현재 시각(ms). 시스템 시계를 바꿔도 거꾸로 가지 않는 단조 시계 */
long long net_now_ms(void);

#endif /* NET_H */

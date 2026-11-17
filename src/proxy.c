/*
 * proxy.c — 요청 하나의 처리 흐름
 *
 * 계획서의 흐름도 중 1단계에서 구현하는 부분:
 *   ① 요청 수신·파싱 ─────────────── 실패 → 400
 *   ② 관제 요청(/__stats 등)? ─────── 4단계에서 구현 (지금은 501)
 *   ③ GET이 아님 (CONNECT 포함) ───── 501
 *   ④ 차단 목록 검사 ──────────────── 2단계
 *   ⑤ CDN 라우터 ──────────────────── 3단계
 *   ⑥ 캐시 조회 ───────────────────── 2단계 (지금은 항상 원서버로)
 *      원서버 요청 (접속 실패 502, 응답 지연 504) → 클라이언트로 중계
 *   ⑦ 기록 → 연결 종료
 */
#include "proxy.h"
#include "http.h"
#include "log.h"
#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RELAY_BUF 65536     /* 원서버 → 클라이언트 중계 버퍼 (64KB) */

/* 설정은 스레드를 만들기 전에 한 번 쓰고 이후로는 읽기만 하므로 락이 필요 없다.
 * (pthread_create 이전에 쓴 값은 새 스레드에서 항상 보인다) */
static struct proxy_config g_cfg;

void proxy_init(const struct proxy_config *cfg)
{
    g_cfg = *cfg;
}

/* 오류 응답을 보내고 기록용 정보를 채운다 */
static void reply_error(int cfd, struct req_log *lg, int status, const char *detail)
{
    http_send_error(cfd, status, detail);
    lg->status = status;
    lg->result = "ERROR";
}

/* 원서버에 요청을 보내고 받은 응답을 그대로 클라이언트에 넘긴다 */
static void relay_from_origin(int cfd, const struct http_request *req, struct req_log *lg)
{
    char out[HTTP_MAX_HEAD + 512];
    int olen = http_build_upstream_request(req, out, sizeof out);
    if (olen < 0) {
        reply_error(cfd, lg, 400, "요청 헤더가 너무 깁니다.");
        return;
    }

    /* 1) 원서버 연결 */
    int ofd = net_connect(req->host, req->port, g_cfg.connect_timeout_ms);
    if (ofd == NET_ERR_TIMEOUT) {
        reply_error(cfd, lg, 504, "원서버 연결 시간이 초과되었습니다.");
        return;
    }
    if (ofd < 0) {
        reply_error(cfd, lg, 502, ofd == NET_ERR_RESOLVE
                                      ? "원서버 주소를 찾을 수 없습니다."
                                      : "원서버에 연결할 수 없습니다.");
        return;
    }
    net_set_timeouts(ofd, g_cfg.io_timeout_ms);

    /* 2) 요청 전송 */
    if (net_write_all(ofd, out, (size_t)olen) < 0) {
        reply_error(cfd, lg, 502, "원서버에 요청을 보내지 못했습니다.");
        close(ofd);
        return;
    }

    /* 3) 응답 중계: 원서버가 연결을 닫을 때(EOF)까지 받은 만큼 바로 넘긴다.
     *    응답 전체를 모았다가 보내지 않으므로 큰 파일도 메모리를 64KB만 쓴다 */
    char buf[RELAY_BUF];
    long long total = 0;
    lg->result = "RELAY";

    for (;;) {
        ssize_t n = recv(ofd, buf, sizeof buf, 0);
        if (n > 0) {
            if (total == 0)
                lg->status = http_parse_status(buf, (size_t)n);   /* 기록용 */
            if (net_write_all(cfd, buf, (size_t)n) < 0) {
                lg->result = "CLIENT_GONE";     /* 클라이언트가 먼저 끊음 */
                break;
            }
            total += n;
            continue;
        }
        if (n == 0) {                           /* EOF = 응답 끝 */
            if (total == 0)
                reply_error(cfd, lg, 502, "원서버가 빈 응답을 보냈습니다.");
            break;
        }
        if (errno == EINTR)
            continue;

        /* n < 0: 수신 실패 */
        int timed_out = (errno == EAGAIN || errno == EWOULDBLOCK);
        if (total == 0) {
            /* 아직 아무것도 보내지 않았으므로 오류 응답을 줄 수 있다 */
            if (timed_out)
                reply_error(cfd, lg, 504, "원서버 응답 시간이 초과되었습니다.");
            else
                reply_error(cfd, lg, 502, "원서버 응답을 받지 못했습니다.");
        } else {
            /* 이미 응답 일부를 보냈다면 상태 코드를 바꿀 수 없다 → 연결을 끊는 수밖에 없다 */
            lg->result = timed_out ? "ORIGIN_TIMEOUT" : "ORIGIN_ERROR";
        }
        break;
    }

    lg->bytes = total;
    close(ofd);
}

void *proxy_handle_connection(void *arg)
{
    struct client_conn *conn = arg;
    int cfd = conn->fd;
    char client[sizeof conn->addr];
    memcpy(client, conn->addr, sizeof client);
    free(conn);

    long long t0 = net_now_ms();
    struct req_log lg = {
        .client = client, .method = "-", .target = "-",
        .status = 0, .bytes = 0, .ms = 0, .result = "ERROR",
    };

    /* 클라이언트가 요청을 보내지 않고 버티거나, 받지 않고 버티는 경우를 대비 */
    net_set_timeouts(cfd, g_cfg.io_timeout_ms);

    /* ① 요청 수신 */
    char head[HTTP_MAX_HEAD];
    ssize_t n = http_read_head(cfd, head, sizeof head);
    if (n == 0 || n == -1)
        goto out_quiet;     /* 요청 없이 끊김·시간 초과: 브라우저의 예비 연결 등. 기록하지 않음 */
    if (n == -2) {
        reply_error(cfd, &lg, 400, "요청 헤더가 너무 큽니다.");
        goto out;
    }

    /* ① 요청 해석 */
    struct http_request req;
    if (http_parse_request(head, &req) < 0) {
        reply_error(cfd, &lg, 400, "요청 형식이 올바르지 않습니다.");
        goto out;
    }
    lg.method = req.method;
    lg.target = req.target;

    /* ② 관제 요청: 프록시 자신에게 온 요청 (4단계에서 /__stats, /__dashboard 구현) */
    if (req.is_control) {
        reply_error(cfd, &lg, 501, "관제 기능은 아직 구현되지 않았습니다.");
        goto out;
    }

    /* ③ GET만 지원한다. HTTPS 프록시 요청(CONNECT)도 여기서 걸러진다 */
    if (strcmp(req.method, "GET") != 0) {
        reply_error(cfd, &lg, 501, "GET 이외의 메서드는 지원하지 않습니다.");
        goto out;
    }

    int ur = http_parse_url(&req);
    if (ur == -2) {
        reply_error(cfd, &lg, 501, "http 이외의 스킴은 지원하지 않습니다.");
        goto out;
    }
    if (ur < 0) {
        reply_error(cfd, &lg, 400, "URL 형식이 올바르지 않습니다.");
        goto out;
    }

    /* ④ 차단 필터 (2단계)   ⑤ CDN 라우터 (3단계)   ⑥ 캐시 조회 (2단계) */

    relay_from_origin(cfd, &req, &lg);

out:
    lg.ms = net_now_ms() - t0;
    log_request(&lg);       /* lg.method, lg.target은 head를 가리키므로 함수가 끝나기 전에 기록 */
out_quiet:
    close(cfd);
    return NULL;
}

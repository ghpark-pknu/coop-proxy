/*
 * proxy.c — 요청 하나의 처리 흐름
 *
 * 계획서의 흐름도 (2단계까지 구현)
 *   ① 요청 수신·파싱 ─────────────── 실패 → 400
 *   ② 관제 요청(/__stats 등)? ─────── 4단계에서 구현 (지금은 501)
 *   ③ GET이 아님 (CONNECT 포함) ───── 501
 *   ④ 차단 목록에 있음 ─────────────── 403
 *   ⑤ CDN 라우터 ──────────────────── 3단계
 *   ⑥ 캐시 조회
 *      ├ HIT, 검증 주기 이내 → 캐시로 바로 응답                         [HIT]
 *      ├ HIT, 검증 주기 지남 → If-Modified-Since로 원서버 확인
 *      │    ├ 304 → 저장 시각 갱신 후 캐시로 응답                        [VALIDATED]
 *      │    ├ 200 → 새 응답을 중계하고 캐시 교체                         [REFRESH]
 *      │    └ 원서버 연결 실패 → 오래된 캐시로 응답                      [STALE]
 *      └ MISS → 원서버 요청 → 중계 → 저장 가능하면 저장                  [MISS]
 *   ⑦ 통계 갱신, 기록 → 연결 종료
 */
#include "proxy.h"
#include "cache.h"
#include "filter.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "stats.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define RELAY_BUF 65536     /* 원서버 → 클라이언트 중계 버퍼 (64KB) */

/* 설정과 캐시 포인터는 스레드를 만들기 전에 한 번 쓰고 이후로는 읽기만 한다 */
static struct proxy_config g_cfg;
static struct cache       *g_cache;     /* NULL이면 캐시 끔 */
static atomic_int          g_active;    /* 진행 중인 연결 수 */

int proxy_init(const struct proxy_config *cfg)
{
    g_cfg = *cfg;
    if (cfg->cache_capacity > 0) {
        g_cache = cache_create(cfg->cache_capacity, cfg->max_object);
        if (g_cache == NULL)
            return -1;
    }
    return 0;
}

void proxy_conn_opened(void)  { atomic_fetch_add(&g_active, 1); }
void proxy_conn_aborted(void) { atomic_fetch_sub(&g_active, 1); }
int  proxy_active_conns(void) { return atomic_load(&g_active); }

/* ------------------------------------------------------------------ */
/* 응답 보내기                                                          */
/* ------------------------------------------------------------------ */

static void reply_error(int cfd, struct req_log *lg, int status, const char *detail)
{
    http_send_error(cfd, status, detail);
    lg->status = status;
    lg->outcome = (status == 403) ? OUT_BLOCKED : OUT_ERROR;
}

/* data(응답 원문)를 보내되, 상태 줄 바로 뒤에 "X-Cache: 결과 from 노드" 헤더를 끼운다.
 * 세 조각을 writev 방식으로 한 번에 보낸다. 성공 0, 실패 -1 */
static int send_with_xcache(int fd, const char *data, size_t len, const char *label)
{
    const char *nl = memchr(data, '\n', len);
    if (nl == NULL)
        return net_write_all(fd, data, len);

    size_t first = (size_t)(nl - data) + 1;     /* 상태 줄 길이 (\r\n 포함) */
    char hdr[96];
    int hlen = snprintf(hdr, sizeof hdr, "X-Cache: %s from %s\r\n", label, g_cfg.node);

    struct iovec iov[3] = {
        { (void *)data,         first       },
        { hdr,                  (size_t)hlen },
        { (void *)(data + first), len - first },
    };
    return net_writev_all(fd, iov, 3);
}

/* 캐시에 있는 객체로 응답한다 */
static void serve_from_cache(int cfd, const struct cache_obj *o, struct req_log *lg,
                             enum outcome outcome)
{
    lg->outcome = outcome;
    lg->status  = http_parse_status(o->data, o->len);
    if (send_with_xcache(cfd, o->data, o->len, outcome_name(outcome)) < 0)
        lg->note = "CLIENT_GONE";
    else
        lg->bytes = (long long)o->len;
}

/* ------------------------------------------------------------------ */
/* 원서버에서 받아 중계하기                                             */
/* ------------------------------------------------------------------ */

enum fetch_kind {
    FETCH_RELAYED,        /* 응답을 클라이언트에 중계했다 (끝까지 또는 중간에 끊김) */
    FETCH_NOT_MODIFIED,   /* 조건부 요청에 304: 클라이언트에는 아무것도 보내지 않았다 */
    FETCH_FAILED          /* 연결·전송·수신 실패: 클라이언트에는 아무것도 보내지 않았다 */
};

struct fetch_result {
    enum fetch_kind kind;
    int             error_status;   /* FETCH_FAILED일 때 보낼 상태 코드 (502, 504) */
    const char     *error_detail;
};

/* 캐시에 넣을 응답 사본을 모으는 버퍼 */
struct collector {
    char  *buf;
    size_t len, cap, limit;
    int    active;      /* 0이면 모으기 중단 (객체 상한 초과 등) */
};

static void collect(struct collector *col, const char *data, size_t n)
{
    if (!col->active)
        return;
    if (col->len + n > col->limit) {           /* 객체 상한을 넘으면 저장 포기 */
        free(col->buf);
        col->buf = NULL;
        col->active = 0;
        return;
    }
    if (col->len + n > col->cap) {
        size_t ncap = col->cap ? col->cap : RELAY_BUF;
        while (ncap < col->len + n)
            ncap *= 2;
        if (ncap > col->limit)
            ncap = col->limit;
        char *nb = realloc(col->buf, ncap);
        if (nb == NULL) {
            free(col->buf);
            col->buf = NULL;
            col->active = 0;
            return;
        }
        col->buf = nb;
        col->cap = ncap;
    }
    memcpy(col->buf + col->len, data, n);
    col->len += n;
}

/* 다 모은 응답이 캐시에 넣을 수 있는 것이면 넣는다 */
static void store_if_cacheable(struct collector *col, const char *url)
{
    struct http_response_info ri;
    int ok = col->active && g_cache != NULL
             && http_parse_response_head(col->buf, col->len, &ri) == 0
             && ri.status == 200                                   /* 정상 응답만 */
             && !ri.no_store                                       /* 저장 금지 아님 */
             && (ri.content_length < 0                             /* 본문이 다 왔는가 */
                 || (long long)(col->len - ri.head_len) == ri.content_length);
    if (ok) {
        cache_insert(g_cache, url, col->buf, col->len, ri.last_modified, net_now_ms());
        col->buf = NULL;            /* 소유권이 캐시로 넘어갔다 */
    } else {
        free(col->buf);
        col->buf = NULL;
    }
}

/* 원서버에 요청을 보내고 응답을 클라이언트에 중계한다.
 *   ims   : NULL이 아니면 If-Modified-Since 조건부 요청
 *   label : 클라이언트에 보낼 X-Cache 값 ("MISS" 또는 "REFRESH")
 * 실패(FETCH_FAILED)했을 때 오류 응답은 호출한 쪽이 결정한다.
 * (검증 실패라면 오류 대신 오래된 캐시로 응답할 수 있으므로) */
static struct fetch_result fetch_and_relay(int cfd, const struct http_request *req,
                                           const char *ims, const char *label,
                                           struct req_log *lg)
{
    struct fetch_result fr = { FETCH_FAILED, 502, "원서버에 연결할 수 없습니다." };

    char out[HTTP_MAX_HEAD + 512];
    int olen = http_build_upstream_request(req, ims, out, sizeof out);
    if (olen < 0) {
        fr.error_status = 400;
        fr.error_detail = "요청 헤더가 너무 깁니다.";
        return fr;
    }

    /* 1) 연결 */
    lg->origin_reqs++;
    int ofd = net_connect(req->host, req->port, g_cfg.connect_timeout_ms);
    if (ofd == NET_ERR_TIMEOUT) {
        fr.error_status = 504;
        fr.error_detail = "원서버 연결 시간이 초과되었습니다.";
        return fr;
    }
    if (ofd < 0) {
        if (ofd == NET_ERR_RESOLVE)
            fr.error_detail = "원서버 주소를 찾을 수 없습니다.";
        return fr;
    }
    net_set_timeouts(ofd, g_cfg.io_timeout_ms);

    /* 2) 요청 전송 */
    if (net_write_all(ofd, out, (size_t)olen) < 0) {
        fr.error_detail = "원서버에 요청을 보내지 못했습니다.";
        close(ofd);
        return fr;
    }

    /* 3) 상태 줄을 다 받을 때까지 읽는다 (304인지 보고 나서 중계 여부를 정해야 하므로) */
    char buf[RELAY_BUF];
    size_t have = 0;
    while (memchr(buf, '\n', have) == NULL && have < sizeof buf) {
        ssize_t n = recv(ofd, buf + have, sizeof buf - have, 0);
        if (n > 0) {
            have += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fr.error_status = 504;
            fr.error_detail = "원서버 응답 시간이 초과되었습니다.";
        } else {
            fr.error_detail = (n == 0 && have == 0) ? "원서버가 빈 응답을 보냈습니다."
                                                    : "원서버 응답을 받지 못했습니다.";
        }
        close(ofd);
        return fr;
    }

    int status = http_parse_status(buf, have);
    if (ims != NULL && status == 304) {
        close(ofd);                         /* 바뀌지 않음 → 캐시 사본을 쓰면 된다 */
        fr.kind = FETCH_NOT_MODIFIED;
        return fr;
    }

    /* 4) 중계 시작: 첫 조각에는 X-Cache 헤더를 끼운다. 동시에 캐시용 사본을 모은다 */
    fr.kind = FETCH_RELAYED;
    lg->status = status;

    struct collector col = { NULL, 0, 0, g_cache ? cache_max_object(g_cache) : 0, g_cache != NULL };
    collect(&col, buf, have);

    long long sent = 0;
    int complete = 0;
    if (send_with_xcache(cfd, buf, have, label) < 0) {
        lg->note = "CLIENT_GONE";
    } else {
        sent += (long long)have;
        for (;;) {
            ssize_t n = recv(ofd, buf, sizeof buf, 0);
            if (n > 0) {
                collect(&col, buf, (size_t)n);
                if (net_write_all(cfd, buf, (size_t)n) < 0) {
                    lg->note = "CLIENT_GONE";
                    break;
                }
                sent += n;
                continue;
            }
            if (n == 0) {                   /* EOF = 응답 끝 */
                complete = 1;
                break;
            }
            if (errno == EINTR)
                continue;
            /* 이미 응답 일부를 보냈으므로 상태 코드를 바꿀 수 없다 → 끊는다 */
            lg->note = (errno == EAGAIN || errno == EWOULDBLOCK) ? "ORIGIN_TIMEOUT" : "ORIGIN_ERROR";
            break;
        }
    }
    close(ofd);
    lg->bytes = sent;

    /* 5) 끝까지 받은 응답만 캐시에 넣는다 (중간에 끊긴 응답은 버림) */
    if (complete)
        store_if_cacheable(&col, req->target);
    else
        free(col.buf);
    return fr;
}

/* ------------------------------------------------------------------ */
/* ⑥ 캐시 조회와 처리                                                   */
/* ------------------------------------------------------------------ */

static void handle_get(int cfd, const struct http_request *req, struct req_log *lg)
{
    long long ttl_ms = (long long)g_cfg.validate_sec * 1000;

    /* 캐시 끔: 매번 원서버로 */
    if (g_cache == NULL) {
        lg->outcome = OUT_MISS;
        struct fetch_result fr = fetch_and_relay(cfd, req, NULL, "MISS", lg);
        if (fr.kind == FETCH_FAILED)
            reply_error(cfd, lg, fr.error_status, fr.error_detail);
        return;
    }

    int expired = 0;
    struct cache_obj *obj = cache_lookup(g_cache, req->target, net_now_ms(), ttl_ms, &expired);

    /* HIT: 검증 주기 이내 */
    if (obj != NULL && !expired) {
        serve_from_cache(cfd, obj, lg, OUT_HIT);
        cache_release(g_cache, obj);
        return;
    }

    /* 검증 주기가 지남 + Last-Modified가 있음 → 조건부 GET으로 확인 */
    if (obj != NULL && obj->last_modified[0] != '\0') {
        lg->outcome = OUT_REFRESH;
        struct fetch_result fr = fetch_and_relay(cfd, req, obj->last_modified, "REFRESH", lg);
        if (fr.kind == FETCH_NOT_MODIFIED) {
            cache_refresh(g_cache, obj, net_now_ms());
            serve_from_cache(cfd, obj, lg, OUT_VALIDATED);
        } else if (fr.kind == FETCH_FAILED) {
            /* 원서버에 확인할 수 없음 → 오류 대신 갖고 있던 사본으로 응답 (가용성 우선) */
            serve_from_cache(cfd, obj, lg, OUT_STALE);
        }
        cache_release(g_cache, obj);
        return;
    }

    /* MISS (또는 Last-Modified가 없어 검증할 수 없는 만료 객체 → 다시 받기) */
    if (obj != NULL)
        cache_release(g_cache, obj);
    lg->outcome = OUT_MISS;
    struct fetch_result fr = fetch_and_relay(cfd, req, NULL, "MISS", lg);
    if (fr.kind == FETCH_FAILED)
        reply_error(cfd, lg, fr.error_status, fr.error_detail);
}

/* ------------------------------------------------------------------ */
/* 작업 스레드                                                          */
/* ------------------------------------------------------------------ */

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
        .status = 0, .bytes = 0, .ms = 0, .outcome = OUT_ERROR, .note = NULL, .origin_reqs = 0,
    };

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

    /* ② 관제 요청 (4단계에서 /__stats, /__dashboard 구현) */
    if (req.is_control) {
        reply_error(cfd, &lg, 501, "관제 기능은 아직 구현되지 않았습니다.");
        goto out;
    }

    /* ③ GET만 지원 */
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

    /* ④ 차단 목록: 원서버에 연결하기 전에 거른다 (DNS 조회도 하지 않음) */
    if (filter_is_blocked(req.host)) {
        reply_error(cfd, &lg, 403, "차단된 사이트입니다.");
        goto out;
    }

    /* ⑤ CDN 라우터 (3단계) */

    /* ⑥ 캐시 조회 → 응답 */
    handle_get(cfd, &req, &lg);

out:
    /* ⑦ 통계와 기록 (lg.method, lg.target은 head를 가리키므로 함수가 끝나기 전에) */
    lg.ms = net_now_ms() - t0;
    stats_record(lg.outcome, lg.origin_reqs, lg.bytes, lg.ms);
    log_request(&lg);
out_quiet:
    close(cfd);
    atomic_fetch_sub(&g_active, 1);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 종료 시 요약                                                         */
/* ------------------------------------------------------------------ */

static void fmt_size(size_t b, char *out, size_t cap)
{
    if (b >= 1024 * 1024)
        snprintf(out, cap, "%.2fMB", (double)b / (1024.0 * 1024.0));
    else
        snprintf(out, cap, "%.1fKB", (double)b / 1024.0);
}

void proxy_report(void)
{
    struct stats_snapshot s;
    stats_get(&s);

    log_info("통계: 요청 %lu | HIT %lu, VALIDATED %lu, STALE %lu, MISS %lu, REFRESH %lu, "
             "BLOCKED %lu, ERROR %lu",
             s.requests, s.count[OUT_HIT], s.count[OUT_VALIDATED], s.count[OUT_STALE],
             s.count[OUT_MISS], s.count[OUT_REFRESH], s.count[OUT_BLOCKED], s.count[OUT_ERROR]);
    log_info("통계: 히트율 %.1f%% | 원서버 요청 %lu | 평균 응답 %.1fms (최대 %lldms) | 전송 %.1fMB",
             stats_hit_ratio(&s) * 100.0, s.origin_requests,
             s.requests ? (double)s.sum_ms / (double)s.requests : 0.0, s.max_ms,
             (double)s.bytes / (1024.0 * 1024.0));

    if (g_cache != NULL) {
        struct cache_usage u;
        cache_get_usage(g_cache, &u);
        char used[32], cap[32];
        fmt_size(u.bytes, used, sizeof used);
        fmt_size(u.capacity, cap, sizeof cap);
        log_info("캐시: 객체 %zu개, %s / %s 사용, LRU 교체 %lu회", u.count, used, cap, u.evictions);
    }
}

void proxy_cleanup(void)
{
    cache_destroy(g_cache);
    g_cache = NULL;
}

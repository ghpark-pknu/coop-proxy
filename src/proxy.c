/*
 * proxy.c — 요청 하나의 처리 흐름
 */
#include "proxy.h"
#include "cache.h"
#include "control.h"
#include "filter.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "router.h"
#include "stats.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define RELAY_BUF 65536     /* 위 → 클라이언트 중계 버퍼 (64KB) */

/* 설정과 캐시 포인터는 스레드를 만들기 전에 한 번 쓰고 이후로는 읽기만 한다 */
static struct proxy_config g_cfg;
static struct cache       *g_cache;     /* NULL이면 캐시 끔 */
static atomic_int          g_active;    /* 진행 중인 연결 수 */
static long long           g_start_us;  /* 시작 시각 (가동 시간 계산용) */

int proxy_init(const struct proxy_config *cfg)
{
    g_cfg = *cfg;
    g_start_us = net_now_us();
    if (cfg->cache_capacity > 0) {
        g_cache = cache_create(cfg->cache_capacity, cfg->max_object, cfg->cache_shards);
        if (g_cache == NULL)
            return -1;
    }
    return 0;
}

void proxy_conn_opened(void)  { atomic_fetch_add(&g_active, 1); }
void proxy_conn_aborted(void) { atomic_fetch_sub(&g_active, 1); }
int  proxy_active_conns(void) { return atomic_load(&g_active); }

const struct proxy_config *proxy_config(void) { return &g_cfg; }
long long proxy_uptime_ms(void) { return (net_now_us() - g_start_us) / 1000; }

struct cache *proxy_cache(void) { return g_cache; }

int proxy_cache_usage(struct cache_usage *u)
{
    if (g_cache == NULL)
        return 0;
    cache_get_usage(g_cache, u);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 응답 보내기                                                          */
/* ------------------------------------------------------------------ */

static void reply_error(int cfd, struct req_log *lg, int status, const char *detail)
{
    http_send_error(cfd, status, detail);
    lg->status  = status;
    lg->outcome = (status == 403) ? OUT_BLOCKED : OUT_ERROR;
}

/* data(응답 원문)를 보내되, 상태 줄 바로 뒤에 "X-Cache: 결과 from 노드" 헤더를 끼운다.
 * 세 조각을 sendmsg 한 번으로 보낸다 (작은 조각을 따로 보내면 Nagle 알고리즘 때문에
 * 지연될 수 있다). 성공 0, 실패 -1 */
static int send_with_xcache(int fd, const char *data, size_t len, const char *label)
{
    const char *nl = memchr(data, '\n', len);
    if (nl == NULL)
        return net_write_all(fd, data, len);

    size_t first = (size_t)(nl - data) + 1;     /* 상태 줄 길이 (\r\n 포함) */
    char hdr[96];
    int hlen = snprintf(hdr, sizeof hdr, "X-Cache: %s from %s\r\n", label, g_cfg.node);

    struct iovec iov[3] = {
        { (void *)data,           first        },
        { hdr,                    (size_t)hlen },
        { (void *)(data + first), len - first  },
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
/* 캐시에 넣을 사본 모으기                                              */
/* ------------------------------------------------------------------ */

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

/* 다 모은 응답이 캐시에 넣을 수 있는 것이면 넣는다. 어느 쪽이든 col->buf는 비워진다 */
static void store_if_cacheable(struct collector *col, const char *url)
{
    struct http_response_info ri;
    int ok = col->active && g_cache != NULL
             && http_parse_response_head(col->buf, col->len, &ri) == 0
             && ri.status == 200                                   /* 정상 응답만 */
             && !ri.no_store                                       /* 저장 금지 아님 */
             && (ri.content_length < 0                             /* 본문이 다 왔는가 */
                 || (long long)(col->len - ri.head_len) == ri.content_length);
    if (ok)
        cache_insert(g_cache, url, col->buf, col->len, ri.last_modified, net_now_ms());
    else
        free(col->buf);
    col->buf = NULL;        /* 저장했으면 소유권이 캐시로 넘어갔고, 아니면 해제했다 */
}

/* ------------------------------------------------------------------ */
/* 위(원서버 또는 다른 노드)에 요청하고 응답을 중계하기                   */
/* ------------------------------------------------------------------ */

enum up_kind {
    UP_RELAYED,         /* 응답을 클라이언트에 중계했다 (끝까지 또는 중간에 끊김) */
    UP_NOT_MODIFIED,    /* 조건부 요청에 304: 클라이언트에는 아무것도 보내지 않았다 */
    UP_FAILED,          /* 연결·전송·수신 실패: 클라이언트에는 아무것도 보내지 않았다 */
    UP_BAD_REQUEST      /* 보낼 요청을 만들 수 없음 (헤더가 너무 김): 연결하지 않았다 */
};

struct up_result {
    enum up_kind kind;
    int          error_status;  /* UP_FAILED·UP_BAD_REQUEST일 때 보낼 상태 코드 */
    const char  *error_detail;
    int          complete;      /* UP_RELAYED: 응답을 EOF까지 다 받아 중계했는가 */
};

struct up_target {
    const char       *host;
    int               port;
    int               absolute;         /* 1: 다른 노드(프록시)에게 절대 URL로 요청 */
    int               io_timeout_ms;
    const char       *ims;              /* NULL이 아니면 If-Modified-Since 조건부 요청 */
    const char       *label;            /* 클라이언트에 보낼 X-Cache 값 */
    struct collector *col;              /* 캐시용 사본을 모을 곳 (NULL이면 모으지 않음) */
};

/* t가 가리키는 곳에 요청을 보내고, 응답을 클라이언트에 중계한다.
 * 실패했을 때(UP_FAILED) 어떻게 할지는 호출한 쪽이 정한다.
 *   - 원서버 MISS라면 502·504 오류 응답
 *   - 원서버 검증이라면 갖고 있던 사본으로 응답 (STALE)
 *   - 다른 노드라면 다음 순위 노드로 (장애 대체)
 * 이것이 가능한 것은 "응답의 첫 줄을 다 받기 전에는 클라이언트에 아무것도 보내지 않기"
 * 때문이다. 일단 보내기 시작하면 되돌릴 수 없다. */
static struct up_result relay_upstream(int cfd, const struct http_request *req,
                                       const struct up_target *t, struct req_log *lg)
{
    struct up_result r = { UP_FAILED, 502, "원서버에 연결할 수 없습니다.", 0 };

    char out[HTTP_MAX_HEAD + 1024];
    struct upstream_opts opts = { t->absolute, t->ims, g_cfg.node };
    int olen = http_build_upstream_request(req, &opts, out, sizeof out);
    if (olen < 0) {
        r.kind = UP_BAD_REQUEST;
        r.error_status = 400;
        r.error_detail = "요청 헤더가 너무 깁니다.";
        return r;
    }

    /* 1) 연결 */
    int ufd = net_connect(t->host, t->port, g_cfg.connect_timeout_ms);
    if (ufd < 0) {
        if (ufd == NET_ERR_TIMEOUT) {
            r.error_status = 504;
            r.error_detail = "원서버 연결 시간이 초과되었습니다.";
        } else if (ufd == NET_ERR_RESOLVE) {
            r.error_detail = "원서버 주소를 찾을 수 없습니다.";
        }
        return r;
    }
    net_set_timeouts(ufd, t->io_timeout_ms);

    /* 2) 요청 전송 */
    if (net_write_all(ufd, out, (size_t)olen) < 0) {
        r.error_detail = "원서버에 요청을 보내지 못했습니다.";
        close(ufd);
        return r;
    }

    /* 3) 상태 줄을 다 받을 때까지 읽는다.
     *    304인지 확인해야 하고, X-Cache를 상태 줄 뒤에 끼워야 하기 때문이다 */
    char buf[RELAY_BUF];
    size_t have = 0;
    while (memchr(buf, '\n', have) == NULL && have < sizeof buf) {
        ssize_t n = recv(ufd, buf + have, sizeof buf - have, 0);
        if (n > 0) {
            have += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            r.error_status = 504;
            r.error_detail = "원서버 응답 시간이 초과되었습니다.";
        } else {
            r.error_detail = (n == 0 && have == 0) ? "원서버가 빈 응답을 보냈습니다."
                                                   : "원서버 응답을 받지 못했습니다.";
        }
        close(ufd);
        return r;
    }

    int status = http_parse_status(buf, have);
    if (t->ims != NULL && status == 304) {
        close(ufd);                         /* 바뀌지 않음 → 캐시 사본을 쓰면 된다 */
        r.kind = UP_NOT_MODIFIED;
        return r;
    }

    /* 4) 여기서부터 클라이언트에 보내기 시작한다 (되돌릴 수 없음) */
    r.kind = UP_RELAYED;
    lg->status = status;
    if (t->col != NULL)
        collect(t->col, buf, have);

    long long sent = 0;
    if (send_with_xcache(cfd, buf, have, t->label) < 0) {
        lg->note = "CLIENT_GONE";
    } else {
        sent += (long long)have;
        for (;;) {
            ssize_t n = recv(ufd, buf, sizeof buf, 0);
            if (n > 0) {
                if (t->col != NULL)
                    collect(t->col, buf, (size_t)n);
                if (net_write_all(cfd, buf, (size_t)n) < 0) {
                    lg->note = "CLIENT_GONE";
                    break;
                }
                sent += n;
                continue;
            }
            if (n == 0) {                   /* EOF = 응답 끝 */
                r.complete = 1;
                break;
            }
            if (errno == EINTR)
                continue;
            /* 이미 응답 일부를 보냈으므로 상태 코드를 바꿀 수 없다 → 끊는다 */
            lg->note = (errno == EAGAIN || errno == EWOULDBLOCK) ? "UPSTREAM_TIMEOUT" : "UPSTREAM_ERROR";
            break;
        }
    }
    close(ufd);
    lg->bytes = sent;
    return r;
}

/* 원서버에서 받아 중계하고, 캐시가 켜져 있으면 저장까지 한다 */
static struct up_result fetch_origin(int cfd, const struct http_request *req, const char *ims,
                                     const char *label, struct req_log *lg)
{
    struct collector col = { NULL, 0, 0, g_cache ? cache_max_object(g_cache) : 0, g_cache != NULL };
    struct up_target t = {
        .host = req->host, .port = req->port, .absolute = 0,
        .io_timeout_ms = g_cfg.io_timeout_ms, .ims = ims, .label = label,
        .col = g_cache ? &col : NULL,
    };

    struct up_result r = relay_upstream(cfd, req, &t, lg);
    if (r.kind != UP_BAD_REQUEST)
        lg->origin_reqs++;              /* 원서버에 연결을 시도했다 */

    if (r.kind == UP_RELAYED && r.complete)
        store_if_cacheable(&col, req->target);  /* 끝까지 받은 응답만 저장 */
    else
        free(col.buf);
    return r;
}

/* ------------------------------------------------------------------ */
/* ⑥ 이 노드에서 처리: 캐시 조회 → 원서버                               */
/* ------------------------------------------------------------------ */

static void handle_local(int cfd, const struct http_request *req, struct req_log *lg)
{
    long long ttl_ms = (long long)g_cfg.validate_sec * 1000;
    struct up_result r;

    /* 캐시 끔: 매번 원서버로 */
    if (g_cache == NULL) {
        lg->outcome = OUT_MISS;
        r = fetch_origin(cfd, req, NULL, "MISS", lg);
        if (r.kind == UP_FAILED || r.kind == UP_BAD_REQUEST)
            reply_error(cfd, lg, r.error_status, r.error_detail);
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
        r = fetch_origin(cfd, req, obj->last_modified, "REFRESH", lg);
        if (r.kind == UP_NOT_MODIFIED) {
            cache_refresh(g_cache, obj, net_now_ms());
            serve_from_cache(cfd, obj, lg, OUT_VALIDATED);
        } else if (r.kind == UP_FAILED) {
            /* 원서버에 확인할 수 없음 → 오류 대신 갖고 있던 사본으로 응답 (가용성 우선) */
            serve_from_cache(cfd, obj, lg, OUT_STALE);
        } else if (r.kind == UP_BAD_REQUEST) {
            reply_error(cfd, lg, r.error_status, r.error_detail);
        }
        cache_release(g_cache, obj);
        return;
    }

    /* MISS (또는 Last-Modified가 없어 검증할 수 없는 만료 객체 → 다시 받기) */
    if (obj != NULL)
        cache_release(g_cache, obj);
    lg->outcome = OUT_MISS;
    r = fetch_origin(cfd, req, NULL, "MISS", lg);
    if (r.kind == UP_FAILED || r.kind == UP_BAD_REQUEST)
        reply_error(cfd, lg, r.error_status, r.error_detail);
}

/* ------------------------------------------------------------------ */
/* ⑤ CDN 라우터                                                         */
/* ------------------------------------------------------------------ */

/* 다른 노드에게 요청을 넘기고 받은 응답을 중계한다 (이 노드는 저장하지 않는다) */
static struct up_result forward_to_peer(int cfd, const struct http_request *req,
                                        const struct node *nd, struct req_log *lg)
{
    struct up_target t = {
        .host = nd->host, .port = nd->port, .absolute = 1,
        /* 받는 노드는 원서버 연결(-c)과 응답 대기(-t)를 다 기다린 뒤에야 502·504를
         * 보낼 수 있다. 그 오류 응답보다 먼저 포기하지 않도록 여유를 더 준다 */
        .io_timeout_ms = g_cfg.io_timeout_ms + g_cfg.connect_timeout_ms + 1000,
        .ims = NULL, .label = "FORWARD", .col = NULL,
    };
    return relay_upstream(cfd, req, &t, lg);
}

/* names 뒤에 이름 하나를 덧붙인다 ("node2" → "node2,node3") */
static void append_name(char *names, size_t cap, const char *name)
{
    size_t len = strlen(names);
    snprintf(names + len, cap - len, "%s%s", len ? "," : "", name);
}

/* 이 요청을 다른 노드가 처리했으면 1, 이 노드가 ⑥에서 처리해야 하면 0 */
static int route_request(int cfd, const struct http_request *req, struct req_log *lg)
{
    if (!router_enabled())
        return 0;                                   /* 독립 프록시 */

    /* 다른 노드가 넘겨준 요청은 다시 넘기지 않는다 (루프 방지) */
    int from = router_via_peer(req);
    if (from >= 0) {
        lg->from_peer = 1;
        snprintf(lg->route, sizeof lg->route, "←%s", router_node(from)->name);
        return 0;
    }

    int order[ROUTER_MAX_NODES];
    int n = router_rank(req->target, order, NULL);
    int self = router_self();
    char failed[2 * ROUTER_NAME_MAX + 2] = "";     /* 연결하지 못한 노드 이름들 */

    /* 1순위(담당), 2순위(대체)까지만 시도한다 */
    for (int i = 0; i < n && i < 2; i++) {
        int idx = order[i];
        if (idx == self) {
            if (i > 0)
                snprintf(lg->route, sizeof lg->route, "대체 (%s 연결 불가)", failed);
            return 0;                               /* 이 노드가 담당 또는 대체 담당 */
        }

        const struct node *nd = router_node(idx);
        if (!router_available(idx, net_now_ms())) {  /* 최근에 실패해서 건너뛰는 중 */
            append_name(failed, sizeof failed, nd->name);
            continue;
        }

        struct up_result r = forward_to_peer(cfd, req, nd, lg);
        if (r.kind == UP_BAD_REQUEST) {
            reply_error(cfd, lg, r.error_status, r.error_detail);
            return 1;
        }
        if (r.kind == UP_RELAYED) {
            if (router_mark_up(idx))
                log_info("%s 복구: 다시 요청을 넘깁니다.", nd->name);
            lg->outcome = OUT_FORWARD;
            if (failed[0])
                snprintf(lg->route, sizeof lg->route, "→%s (%s 연결 불가)", nd->name, failed);
            else
                snprintf(lg->route, sizeof lg->route, "→%s", nd->name);
            return 1;
        }

        /* UP_FAILED: 응답을 한 바이트도 받기 전에 실패 → 클라이언트에 보낸 것이 없으므로
         * 다음 순위로 넘어갈 수 있다 */
        lg->peer_failures++;
        if (router_mark_down(idx, net_now_ms(), g_cfg.peer_retry_ms))
            log_info("%s(%s:%d)에 요청을 넘길 수 없습니다. %dms 동안 건너뜁니다.",
                     nd->name, nd->host, nd->port, g_cfg.peer_retry_ms);
        append_name(failed, sizeof failed, nd->name);
    }

    /* 1·2순위가 모두 다른 노드이고 둘 다 실패 → 이 노드가 직접 처리 */
    snprintf(lg->route, sizeof lg->route, "직접 (%s 연결 불가)", failed);
    return 0;
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

    long long t0 = net_now_us();
    struct req_log lg = {
        .client = client, .method = "-", .target = "-",
        .outcome = OUT_ERROR,       /* 나머지 필드는 0 / NULL / 빈 문자열 */
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

    /* ② 관제 요청: 프록시 자신에게 온 요청 (/__stats, /__dashboard).
     *    대시보드가 1초마다 조회하므로 통계와 기록에는 넣지 않는다 */
    if (req.is_control) {
        control_handle(cfd, &req);
        goto out_quiet;
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

    /* ⑤ CDN 라우터 → ⑥ 이 노드에서 처리 */
    if (!route_request(cfd, &req, &lg))
        handle_local(cfd, &req, &lg);

out:
    /* ⑦ 통계와 기록 (lg.method, lg.target은 head를 가리키므로 함수가 끝나기 전에) */
    lg.us = net_now_us() - t0;
    {
        struct stats_event ev = {
            .outcome = lg.outcome, .origin_requests = lg.origin_reqs,
            .peer_failures = lg.peer_failures, .from_peer = lg.from_peer,
            .bytes = lg.bytes, .us = lg.us,
        };
        stats_record(&ev);
    }
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

/* 락 경합 요약 한 줄. 예: "락: 캐시(샤드 8개) | 획득 120000회, 경합 310회 (0.26%), 대기 합 4.2ms" */
static void log_lock(const char *name, int shards, const struct lock_stats *ls)
{
    char what[64];
    if (shards > 1)
        snprintf(what, sizeof what, "%s(샤드 %d개)", name, shards);
    else
        snprintf(what, sizeof what, "%s(mutex 1개)", name);
    log_info("%s | 획득 %lu회, 경합 %lu회 (%.2f%%), 대기 합 %.1fms",
             what, ls->acquires, ls->contended,
             ls->acquires ? 100.0 * (double)ls->contended / (double)ls->acquires : 0.0,
             (double)ls->wait_ns / 1e6);
}

void proxy_report(void)
{
    struct stats_snapshot s;
    stats_get(&s);

    log_info("통계: 요청 %lu | HIT %lu, VALIDATED %lu, STALE %lu, MISS %lu, REFRESH %lu, "
             "FORWARD %lu, BLOCKED %lu, ERROR %lu",
             s.requests, s.count[OUT_HIT], s.count[OUT_VALIDATED], s.count[OUT_STALE],
             s.count[OUT_MISS], s.count[OUT_REFRESH], s.count[OUT_FORWARD],
             s.count[OUT_BLOCKED], s.count[OUT_ERROR]);
    log_info("통계: 히트율 %.1f%% | 원서버 요청 %lu | 클라이언트 응답 평균 %.2fms (최대 %.1fms) | 전송 %.1fMB",
             stats_hit_ratio(&s) * 100.0, s.origin_requests,
             s.client.count ? (double)s.client.sum_us / (double)s.client.count / 1000.0 : 0.0,
             (double)s.client.max_us / 1000.0, (double)s.bytes / (1024.0 * 1024.0));
    if (router_enabled())
        log_info("클러스터: 넘긴 요청 %lu | 넘겨받은 요청 %lu | 다른 노드 연결 실패 %lu회",
                 s.count[OUT_FORWARD], s.peer.count, s.peer_failures);

    if (g_cache != NULL) {
        struct cache_usage u;
        cache_get_usage(g_cache, &u);
        char used[32], cap[32];
        fmt_size(u.bytes, used, sizeof used);
        fmt_size(u.capacity, cap, sizeof cap);
        log_info("캐시: 객체 %zu개, %s / %s 사용, LRU 교체 %lu회", u.count, used, cap, u.evictions);
        log_lock("락: 캐시", u.shards, &u.lock);
    }
    log_lock("락: 통계", 1, &s.lock);
}

void proxy_cleanup(void)
{
    cache_destroy(g_cache);
    g_cache = NULL;
}

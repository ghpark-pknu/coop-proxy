/*
 * control.c — 관제 요청 처리: /__stats(JSON), /__dashboard(HTML)
 */
#include "control.h"
#include "cache.h"
#include "http.h"
#include "net.h"
#include "proxy.h"
#include "router.h"
#include "stats.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define JSON_MAX      32768       /* /__stats 응답 최대 크기 (실제로는 수 KB) */
#define DASHBOARD_MAX (1 << 20)   /* 관제 화면 파일 최대 크기 1MB */

/* ------------------------------------------------------------------ */
/* 응답 보내기                                                          */
/* ------------------------------------------------------------------ */

/* 헤더와 본문을 한 번에 보낸다. extra_headers는 "\r\n"으로 끝나는 줄들 (없으면 "") */
static void send_response(int fd, int status, const char *content_type,
                          const char *extra_headers, const char *body, size_t body_len)
{
    /* 204 No Content에는 Content-Length를 보내면 안 된다 (RFC 9110 8.6) */
    char length[48] = "";
    if (status != 204)
        snprintf(length, sizeof length, "Content-Length: %zu\r\n", body_len);

    char head[1024];
    int hlen = snprintf(head, sizeof head,
                        "HTTP/1.0 %d %s\r\n"
                        "%s%s%s"
                        "%s"
                        "Cache-Control: no-store\r\n"
                        "Connection: close\r\n"
                        "%s"
                        "\r\n",
                        status, http_reason(status),
                        content_type ? "Content-Type: " : "", content_type ? content_type : "",
                        content_type ? "\r\n" : "",
                        length, extra_headers);
    if (hlen < 0 || (size_t)hlen >= sizeof head)
        return;
    struct iovec iov[2] = {
        { head,         (size_t)hlen },
        { (void *)body, body_len     },
    };
    net_writev_all(fd, iov, body_len ? 2 : 1);
}

/* ------------------------------------------------------------------ */
/* JSON 만들기                                                          */
/* ------------------------------------------------------------------ */

struct jbuf {
    char   data[JSON_MAX];
    size_t len;
    int    overflow;        /* 공간이 모자라 잘렸으면 1 → 500으로 응답 */
};

static void jprintf(struct jbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jprintf(struct jbuf *b, const char *fmt, ...)
{
    if (b->overflow)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->data + b->len, sizeof b->data - b->len, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= sizeof b->data - b->len)
        b->overflow = 1;
    else
        b->len += (size_t)w;
}

/* JSON 문자열: 큰따옴표로 감싸고 ", \, 제어 문자를 이스케이프한다 */
static void jstr(struct jbuf *b, const char *s)
{
    jprintf(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            jprintf(b, "\\%c", c);
        else if (c < 0x20)
            jprintf(b, "\\u%04x", c);
        else
            jprintf(b, "%c", c);
    }
    jprintf(b, "\"");
}

static void jlatency(struct jbuf *b, const char *name, const struct latency_stats *l)
{
    jprintf(b, "\"%s\":{\"count\":%lu,\"sum_us\":%llu,\"max_us\":%lld,\"hist\":[",
            name, l->count, l->sum_us, l->max_us);
    for (int i = 0; i < STATS_NBUCKETS; i++)
        jprintf(b, "%s%lu", i ? "," : "", l->hist[i]);
    jprintf(b, "]}");
}

/* "이름":{"acquires":획득, "contended":경합, "wait_us":기다린 시간 합(마이크로초)} */
static void jlock(struct jbuf *b, const char *name, const struct lock_stats *ls)
{
    jprintf(b, "\"%s\":{\"acquires\":%lu,\"contended\":%lu,\"wait_us\":%llu}",
            name, ls->acquires, ls->contended, ls->wait_ns / 1000ULL);
}

static void send_stats(int cfd)
{
    const struct proxy_config *cfg = proxy_config();
    struct stats_snapshot s;
    stats_get(&s);
    struct cache_usage u;
    int cache_on = proxy_cache_usage(&u);

    struct jbuf *b = calloc(1, sizeof *b);     /* 32KB 버퍼는 스택 대신 힙에 둔다 */
    if (b == NULL) {
        http_send_error(cfd, 500, "메모리가 부족합니다.");
        return;
    }

    jprintf(b, "{\"node\":");
    jstr(b, cfg->node);
    jprintf(b, ",\"mode\":\"%s\",\"uptime_ms\":%lld,\"active_connections\":%d",
            router_enabled() ? "cluster" : "standalone", proxy_uptime_ms(), proxy_active_conns());

    jprintf(b, ",\"requests\":{\"client\":%lu,\"peer\":%lu}", s.client.count, s.peer.count);

    jprintf(b, ",\"outcomes\":{");
    for (int i = 0; i < OUT_COUNT; i++)
        jprintf(b, "%s\"%s\":%lu", i ? "," : "", outcome_name((enum outcome)i), s.count[i]);
    jprintf(b, "}");

    jprintf(b, ",\"origin_requests\":%lu,\"peer_failures\":%lu,\"bytes\":%llu",
            s.origin_requests, s.peer_failures, s.bytes);

    /* 힙 메모리 (glibc mallinfo2, 모든 아레나의 합). 8단계 장시간 부하 시험에서 누수를 판단하는 데 쓴다.
     *   heap_in_use : 지금 malloc으로 받아 아직 free하지 않은 바이트 (누수가 있으면 계속 는다)
     *   heap_system : 할당기가 운영체제에서 받아 둔 바이트. free한 메모리도 다음 malloc을 위해
     *                 쥐고 있으므로 heap_in_use보다 크고, RSS(실제 차지한 물리 메모리)에 포함된다.
     * 스레드마다 다른 아레나(할당 구역)를 쓸 수 있어서, 크기가 제각각인 객체를 오래 할당·해제하면
     * heap_system은 조각난 빈 공간 때문에 천천히 늘 수 있다. 이것은 누수가 아니다.
     * 새니타이저 빌드(asan·tsan)는 자기 할당기를 쓰므로 둘 다 0으로 나온다 */
    struct mallinfo2 mi = mallinfo2();
    jprintf(b, ",\"memory\":{\"heap_in_use\":%zu,\"heap_system\":%zu}",
            mi.uordblks + mi.hblkhd, mi.arena + mi.hblkhd);

    jprintf(b, ",\"latency\":{\"bounds_us\":[");
    for (int i = 0; i < STATS_NBUCKETS - 1; i++)
        jprintf(b, "%s%lld", i ? "," : "", STATS_BOUNDS_US[i]);
    jprintf(b, "],");
    jlatency(b, "client", &s.client);
    jprintf(b, ",");
    jlatency(b, "peer", &s.peer);
    jprintf(b, "}");

    if (cache_on) {
        jprintf(b, ",\"cache\":{\"enabled\":true,\"objects\":%zu,\"bytes\":%zu,\"capacity\":%zu,"
                   "\"max_object\":%zu,\"evictions\":%lu,\"shards\":%d",
                u.count, u.bytes, u.capacity, cfg->max_object, u.evictions, u.shards);
        /* 샤드별 객체 수·바이트: URL이 샤드에 고르게 퍼지는지 확인용 */
        struct cache *c = proxy_cache();
        struct cache_usage su[CACHE_MAX_SHARDS];
        for (int i = 0; i < u.shards; i++)
            cache_get_shard_usage(c, i, &su[i]);
        jprintf(b, ",\"shard_objects\":[");
        for (int i = 0; i < u.shards; i++)
            jprintf(b, "%s%zu", i ? "," : "", su[i].count);
        jprintf(b, "],\"shard_bytes\":[");
        for (int i = 0; i < u.shards; i++)
            jprintf(b, "%s%zu", i ? "," : "", su[i].bytes);
        jprintf(b, "]}");
    } else {
        jprintf(b, ",\"cache\":{\"enabled\":false,\"objects\":0,\"bytes\":0,\"capacity\":0,"
                   "\"max_object\":0,\"evictions\":0,\"shards\":0,\"shard_objects\":[],\"shard_bytes\":[]}");
        memset(&u.lock, 0, sizeof u.lock);
    }

    /* 락 경합 (누적). 캐시: 모든 샤드의 합 / 통계: 요청마다 한 번 잡는 전역 락 */
    jprintf(b, ",\"locks\":{");
    jlock(b, "cache", &u.lock);
    jprintf(b, ",");
    jlock(b, "stats", &s.lock);
    jprintf(b, "}");

    jprintf(b, ",\"config\":{\"validate_sec\":%d,\"peer_retry_ms\":%d,\"cache_shards\":%d}",
            cfg->validate_sec, cfg->peer_retry_ms, cfg->cache_capacity > 0 ? cfg->cache_shards : 0);

    jprintf(b, ",\"cluster\":[");
    long long now = net_now_ms();
    for (int i = 0; i < router_count(); i++) {
        const struct node *nd = router_node(i);
        jprintf(b, "%s{\"name\":", i ? "," : "");
        jstr(b, nd->name);
        jprintf(b, ",\"host\":");
        jstr(b, nd->host);
        jprintf(b, ",\"port\":%d,\"self\":%s,\"available\":%s}", nd->port,
                i == router_self() ? "true" : "false",
                router_available(i, now) ? "true" : "false");
    }
    jprintf(b, "]}\n");

    if (b->overflow)
        http_send_error(cfd, 500, "통계 응답이 너무 큽니다.");
    else
        /* 대시보드는 다른 포트(다른 출처)의 노드에서도 이 응답을 읽어야 하므로
         * 모든 출처의 읽기를 허용한다 (CORS) */
        send_response(cfd, 200, "application/json; charset=utf-8",
                      "Access-Control-Allow-Origin: *\r\n", b->data, b->len);
    free(b);
}

/* ------------------------------------------------------------------ */
/* 관제 화면                                                            */
/* ------------------------------------------------------------------ */

/* 요청마다 파일을 새로 읽는다: 화면 파일을 고친 뒤 브라우저를 새로고침하면 바로 반영된다 */
static void send_dashboard(int cfd)
{
    const char *path = proxy_config()->dashboard_path;
    if (path[0] == '\0') {
        http_send_error(cfd, 404, "관제 화면 파일(web/dashboard.html)을 찾지 못했습니다. "
                                  "프로젝트 폴더에서 프록시를 실행하세요.");
        return;
    }
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > DASHBOARD_MAX) {
        if (fd >= 0)
            close(fd);
        http_send_error(cfd, 404, "관제 화면 파일을 읽을 수 없습니다.");
        return;
    }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size);
    size_t got = 0;
    while (buf != NULL && got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (buf == NULL || got != size) {
        free(buf);
        http_send_error(cfd, 500, "관제 화면 파일을 읽는 중 오류가 났습니다.");
        return;
    }
    send_response(cfd, 200, "text/html; charset=utf-8", "", buf, size);
    free(buf);
}

/* ------------------------------------------------------------------ */

int control_handle(int cfd, const struct http_request *req)
{
    if (strcmp(req->method, "GET") != 0) {
        http_send_error(cfd, 501, "관제 경로는 GET만 지원합니다.");
        return 501;
    }

    /* 쿼리 문자열(?nodes=...)은 떼고 경로만 비교한다 */
    size_t plen = strcspn(req->target, "?");
#define IS(p) (plen == sizeof(p) - 1 && strncmp(req->target, p, plen) == 0)

    if (IS("/__stats")) {
        send_stats(cfd);
        return 200;
    }
    if (IS("/__dashboard")) {
        send_dashboard(cfd);
        return 200;
    }
    if (IS("/")) {
        send_response(cfd, 302, NULL, "Location: /__dashboard\r\n", "", 0);
        return 302;
    }
    if (IS("/favicon.ico")) {
        send_response(cfd, 204, NULL, "", "", 0);
        return 204;
    }
#undef IS
    http_send_error(cfd, 404, "없는 관제 경로입니다. /__dashboard 또는 /__stats를 쓰세요.");
    return 404;
}

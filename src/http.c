/*
 * http.c — HTTP 메시지 읽기·해석·생성
 */
#define _GNU_SOURCE /* memmem() */
#include "http.h"
#include "net.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------ */
/* 요청 헤더 읽기                                                       */
/* ------------------------------------------------------------------ */

/* buf[0..len)에서 헤더 끝 표시를 찾아 "헤더 끝 바로 다음 위치"를 반환한다.
 * 표준은 "\r\n\r\n"이지만, nc로 손으로 입력하면 "\n"만 들어오므로
 * "\n\n"도 받아 준다 (RFC 9112도 LF만 쓴 줄 끝을 허용할 수 있다고 함) */
static size_t find_head_end(const char *buf, size_t len)
{
    const char *crlf = memmem(buf, len, "\r\n\r\n", 4);
    const char *lf   = memmem(buf, len, "\n\n", 2);
    if (crlf && (!lf || crlf < lf))
        return (size_t)(crlf - buf) + 4;
    if (lf)
        return (size_t)(lf - buf) + 2;
    return 0;
}

ssize_t http_read_head(int fd, char *buf, size_t cap)
{
    size_t len = 0;

    /* TCP는 "바이트의 흐름"이라 요청이 한 번의 recv로 다 온다는 보장이 없다.
     * 헤더 끝이 보일 때까지 이어서 받는다 */
    while (len < cap - 1) {
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n == 0)
            return len == 0 ? 0 : -1;       /* 0: 그냥 닫음, -1: 헤더 도중에 끊김 */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;                      /* EAGAIN = SO_RCVTIMEO 시간 초과 */
        }

        /* 헤더 끝 표시가 두 번의 recv에 걸쳐 나뉘어 올 수 있으므로
         * 새로 받은 부분보다 3바이트 앞에서부터 찾는다 */
        size_t from = len >= 3 ? len - 3 : 0;
        len += (size_t)n;

        size_t end = find_head_end(buf + from, len - from);
        if (end > 0) {
            end += from;
            buf[end] = '\0';    /* 헤더 뒤(본문)는 버린다. GET에는 본문이 없다 */
            return (ssize_t)end;
        }
    }
    return -2;
}

/* ------------------------------------------------------------------ */
/* 요청 해석                                                            */
/* ------------------------------------------------------------------ */

/* line이 가리키는 줄을 '\0'으로 끝내고(끝의 \r도 제거) 다음 줄의 시작을 반환한다 */
static char *next_line(char *line)
{
    char *nl = strchr(line, '\n');
    if (nl == NULL)
        return NULL;
    *nl = '\0';
    if (nl > line && nl[-1] == '\r')
        nl[-1] = '\0';
    return nl + 1;
}

static int is_valid_method(const char *m)
{
    if (*m == '\0')
        return 0;
    for (; *m; m++)
        if (!isupper((unsigned char)*m))
            return 0;
    return 1;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

int http_parse_request(char *buf, struct http_request *req)
{
    memset(req, 0, sizeof *req);

    /* 1) 요청 라인: METHOD SP TARGET SP VERSION */
    char *line = buf;
    char *next = next_line(line);
    if (next == NULL)
        return -1;

    char *sp1 = strchr(line, ' ');
    if (sp1 == NULL)
        return -1;
    *sp1 = '\0';
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (sp2 == NULL)
        return -1;
    *sp2 = '\0';
    char *version = sp2 + 1;

    if (!is_valid_method(line) || *target == '\0')
        return -1;
    /* "HTTP/1.0" 또는 "HTTP/1.1"만 받는다 (공백이 더 있으면 형식 오류) */
    if (strncmp(version, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)version[7])
        || version[8] != '\0')
        return -1;

    req->method     = line;
    req->target     = target;
    req->version    = version;
    req->is_control = (target[0] == '/');

    /* 2) 헤더: "이름: 값" 이 빈 줄까지 이어진다 */
    for (line = next; line != NULL; line = next) {
        next = next_line(line);
        if (*line == '\0')
            break;                              /* 빈 줄 = 헤더 끝 */
        if (*line == ' ' || *line == '\t')
            return -1;                          /* 옛 형식의 줄 이어쓰기(obs-fold)는 거부 */

        char *colon = strchr(line, ':');
        if (colon == NULL || colon == line)
            return -1;
        *colon = '\0';
        if (strpbrk(line, " \t") != NULL)
            return -1;                          /* 이름과 ':' 사이에 공백 금지 (RFC 9112) */

        if (req->nheaders == HTTP_MAX_HEADERS)
            return -1;
        req->headers[req->nheaders].name  = line;
        req->headers[req->nheaders].value = trim(colon + 1);
        req->nheaders++;
    }
    return 0;
}

static int is_host_char(char c)
{
    return isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_';
}

int http_parse_url(struct http_request *req)
{
    const char *p = req->target;

    if (strncasecmp(p, "http://", 7) != 0)
        return strstr(p, "://") != NULL ? -2 : -1;
    p += 7;

    /* 호스트: 영문·숫자·'.'·'-'·'_'만 허용 (IPv6 주소 표기 [::1]은 지원하지 않음) */
    const char *h = p;
    while (is_host_char(*p))
        p++;
    size_t hlen = (size_t)(p - h);
    if (hlen == 0 || hlen >= HTTP_MAX_HOST)
        return -1;
    memcpy(req->host, h, hlen);
    req->host[hlen] = '\0';

    /* 포트: 없으면 80 */
    req->port = 80;
    if (*p == ':') {
        p++;
        if (!isdigit((unsigned char)*p))
            return -1;
        char *end;
        long port = strtol(p, &end, 10);
        if (port < 1 || port > 65535)
            return -1;
        req->port = (int)port;
        p = end;
    }

    /* 경로: 없으면 "/" */
    if (*p == '\0')
        req->path = "/";
    else if (*p == '/')
        req->path = p;
    else
        return -1;          /* 예: "http://host?x" 처럼 경로 없이 쿼리가 오는 경우 */
    return 0;
}

/* ------------------------------------------------------------------ */
/* 원서버로 보낼 요청 만들기                                            */
/* ------------------------------------------------------------------ */

/* hop-by-hop 헤더: "바로 옆 연결"에만 의미가 있어서 다음 구간으로 넘기면 안 되는 헤더.
 * 예) Proxy-Connection: Keep-Alive 는 "클라이언트↔프록시" 연결에 대한 이야기다.
 * Host는 URL에서 다시 만들어 넣으므로 같이 뺀다 */
static int is_hop_by_hop(const char *name)
{
    static const char *const hop[] = {
        "Connection", "Proxy-Connection", "Keep-Alive", "TE", "Trailer",
        "Transfer-Encoding", "Upgrade", "Proxy-Authorization", "Proxy-Authenticate",
        "Host", NULL
    };
    for (int i = 0; hop[i] != NULL; i++)
        if (strcasecmp(name, hop[i]) == 0)
            return 1;
    return 0;
}

/* "Connection: keep-alive, X-Foo" 처럼 Connection 헤더 값에 이름이 나열된
 * 헤더도 hop-by-hop으로 취급해야 한다 (RFC 9110 7.6.1) */
static int listed_in_connection(const struct http_request *req, const char *name)
{
    size_t nlen = strlen(name);
    for (int i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, "Connection") != 0)
            continue;
        const char *v = req->headers[i].value;
        while (*v) {
            while (*v == ' ' || *v == '\t' || *v == ',')
                v++;
            size_t tlen = strcspn(v, " \t,");
            if (tlen == nlen && strncasecmp(v, name, nlen) == 0)
                return 1;
            v += tlen;
        }
    }
    return 0;
}

/* 클라이언트의 조건부 헤더. 그대로 넘기면 원서버가 304(본문 없음)를 보낼 수 있어
 * 캐시에 저장할 수 없으므로, 프록시는 이 헤더를 빼고 요청한다 */
static int is_client_conditional(const char *name)
{
    return strcasecmp(name, "If-Modified-Since") == 0
        || strcasecmp(name, "If-None-Match") == 0;
}

int http_build_upstream_request(const struct http_request *req, const struct upstream_opts *o,
                                char *out, size_t cap)
{
    size_t n = 0;
    int w;

/* snprintf로 out 뒤에 이어 쓰고, 공간이 모자라면 -1 반환 */
#define APPEND(...)                                              \
    do {                                                         \
        w = snprintf(out + n, cap - n, __VA_ARGS__);             \
        if (w < 0 || (size_t)w >= cap - n)                       \
            return -1;                                           \
        n += (size_t)w;                                          \
    } while (0)

    /* HTTP/1.0 + Connection: close 로 요청한다.
     * → 상대가 응답을 다 보내면 연결을 닫으므로 "EOF = 응답 끝"으로 판단할 수 있다.
     * 다른 노드는 프록시이므로 절대 URL 그대로, 원서버에는 경로만 보낸다 */
    APPEND("GET %s HTTP/1.0\r\n", o->absolute ? req->target : req->path);
    if (req->port == 80)
        APPEND("Host: %s\r\n", req->host);
    else
        APPEND("Host: %s:%d\r\n", req->host, req->port);
    APPEND("Connection: close\r\n");
    if (o->ims != NULL)
        APPEND("If-Modified-Since: %s\r\n", o->ims);    /* 캐시 검증용 조건부 GET */

    /* Via: 지금까지 거친 프록시 목록 뒤에 이 노드를 덧붙인다.
     *   클라이언트 → node1 (HTTP/1.1로 받음) → node2 (HTTP/1.0으로 받음) → 원서버
     *   원서버가 받는 값: "Via: 1.1 node1, 1.0 node2"
     * req->version은 "HTTP/1.x"로 검증되어 있으므로 +5 하면 "1.x" */
    if (o->via_name != NULL) {
        APPEND("Via: ");
        for (int i = 0; i < req->nheaders; i++)
            if (strcasecmp(req->headers[i].name, "Via") == 0 && req->headers[i].value[0] != '\0')
                APPEND("%s, ", req->headers[i].value);
        APPEND("%s %s\r\n", req->version + 5, o->via_name);
    }

    for (int i = 0; i < req->nheaders; i++) {
        const char *name = req->headers[i].name;
        if (is_hop_by_hop(name) || listed_in_connection(req, name) || is_client_conditional(name))
            continue;
        if (o->via_name != NULL && strcasecmp(name, "Via") == 0)
            continue;                                   /* 위에서 합쳐서 보냈다 */
        APPEND("%s: %s\r\n", name, req->headers[i].value);
    }
    APPEND("\r\n");
#undef APPEND
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* 응답 관련                                                            */
/* ------------------------------------------------------------------ */

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    default:  return "Error";
    }
}

void http_send_error(int fd, int status, const char *detail)
{
    const char *reason = http_reason(status);
    char body[1024];
    int blen = snprintf(body, sizeof body,
                        "<!DOCTYPE html>\n"
                        "<html><head><meta charset=\"utf-8\"><title>%d %s</title></head>\n"
                        "<body><h1>%d %s</h1><p>%s</p><hr><small>coop-proxy</small></body></html>\n",
                        status, reason, status, reason, detail ? detail : "");
    if (blen < 0)
        return;
    if ((size_t)blen >= sizeof body)
        blen = (int)sizeof body - 1;

    char resp[1536];
    int rlen = snprintf(resp, sizeof resp,
                        "HTTP/1.0 %d %s\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "%s",
                        status, reason, blen, body);
    if (rlen < 0)
        return;
    if ((size_t)rlen >= sizeof resp)
        rlen = (int)sizeof resp - 1;
    net_write_all(fd, resp, (size_t)rlen);
}

int http_parse_response_head(const char *buf, size_t len, struct http_response_info *ri)
{
    memset(ri, 0, sizeof *ri);
    ri->content_length = -1;

    size_t head_len = find_head_end(buf, len);
    if (head_len == 0)
        return -1;
    ri->head_len = head_len;

    ri->status = http_parse_status(buf, len);
    if (ri->status == 0)
        return -1;

    /* 상태 줄 다음 줄부터 헤더를 한 줄씩 본다 (버퍼를 수정하지 않고 길이로 다룸) */
    const char *p   = memchr(buf, '\n', head_len);
    const char *end = buf + head_len;
    if (p == NULL)
        return -1;
    p++;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (nl == NULL)
            break;
        size_t llen = (size_t)(nl - p);
        if (llen > 0 && p[llen - 1] == '\r')
            llen--;
        if (llen == 0)
            break;                                  /* 빈 줄 = 헤더 끝 */

        const char *colon = memchr(p, ':', llen);
        if (colon != NULL) {
            size_t nlen = (size_t)(colon - p);
            const char *v = colon + 1;
            const char *vend = p + llen;
            while (v < vend && (*v == ' ' || *v == '\t'))
                v++;
            size_t vlen = (size_t)(vend - v);

            if (nlen == 14 && strncasecmp(p, "Content-Length", 14) == 0) {
                ri->content_length = strtoll(v, NULL, 10);
            } else if (nlen == 13 && strncasecmp(p, "Last-Modified", 13) == 0) {
                if (vlen >= sizeof ri->last_modified)
                    vlen = sizeof ri->last_modified - 1;
                memcpy(ri->last_modified, v, vlen);
                ri->last_modified[vlen] = '\0';
            } else if (nlen == 13 && strncasecmp(p, "Cache-Control", 13) == 0) {
                char val[256];
                size_t n = vlen < sizeof val - 1 ? vlen : sizeof val - 1;
                memcpy(val, v, n);
                val[n] = '\0';
                if (strcasestr(val, "no-store") || strcasestr(val, "private"))
                    ri->no_store = 1;
            }
        }
        p = nl + 1;
    }
    return 0;
}

int http_parse_status(const char *buf, size_t len)
{
    /* "HTTP/1.x DDD" → 최소 12바이트 */
    if (len < 12 || memcmp(buf, "HTTP/1.", 7) != 0 || buf[8] != ' ')
        return 0;
    if (!isdigit((unsigned char)buf[9]) || !isdigit((unsigned char)buf[10])
        || !isdigit((unsigned char)buf[11]))
        return 0;
    return (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
}

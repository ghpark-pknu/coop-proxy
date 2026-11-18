/*
 * http.h — HTTP 메시지 읽기·해석·생성
 *
 * 프록시가 받는 요청의 모양 (요청 대상이 "절대 URL"인 것이 일반 서버와 다른 점)
 *
 *     GET http://127.0.0.1:9000/a.html HTTP/1.1\r\n     ← 요청 라인
 *     Host: 127.0.0.1:9000\r\n                          ← 헤더들
 *     User-Agent: curl/8.5.0\r\n
 *     Proxy-Connection: Keep-Alive\r\n
 *     \r\n                                              ← 빈 줄 = 헤더 끝
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/types.h>

#define HTTP_MAX_HEAD    16384   /* 요청 라인 + 헤더 전체의 최대 크기 (바이트) */
#define HTTP_MAX_HEADERS 64      /* 헤더 최대 개수 */
#define HTTP_MAX_HOST    256     /* 호스트 이름 최대 길이 */

/* 헤더 한 줄. name, value는 요청 버퍼 안을 가리킨다 (복사하지 않음) */
struct http_header {
    const char *name;
    const char *value;
};

struct http_request {
    /* 요청 라인 (버퍼 안을 가리킴) */
    const char *method;          /* "GET" */
    const char *target;          /* "http://127.0.0.1:9000/a.html" 또는 "/__stats" */
    const char *version;         /* "HTTP/1.1" */
    int         is_control;      /* target이 '/'로 시작하면 1 (프록시 자신에게 온 관제 요청) */

    /* http_parse_url()이 절대 URL을 분해한 결과 */
    char        host[HTTP_MAX_HOST];
    int         port;
    const char *path;            /* "/a.html" (쿼리 문자열 포함) */

    /* 헤더 */
    struct http_header headers[HTTP_MAX_HEADERS];
    int         nheaders;
};

/* 클라이언트 소켓에서 헤더 끝(빈 줄)까지 읽어 buf에 넣고 '\0'으로 끝낸다.
 * 반환: 읽은 헤더 길이(>0)
 *       0  = 아무것도 보내지 않고 연결을 닫음
 *       -1 = 수신 오류·제한 시간 초과·헤더 도중 끊김
 *       -2 = 헤더가 cap보다 큼 */
ssize_t http_read_head(int fd, char *buf, size_t cap);

/* buf의 요청 라인과 헤더를 해석해 req를 채운다. buf를 직접 수정한다
 * (구분자 자리에 '\0'을 넣음). 성공 0, 형식 오류 -1 */
int http_parse_request(char *buf, struct http_request *req);

/* req->target("http://host[:port]/path")을 host, port, path로 분해한다.
 * 성공 0, 형식 오류 -1, http가 아닌 스킴(https 등) -2 */
int http_parse_url(struct http_request *req);

/* 위(원서버 또는 다른 노드)로 보낼 요청을 어떻게 만들지 */
struct upstream_opts {
    int         absolute;   /* 1: 요청 대상을 절대 URL 그대로 (다른 노드 = 프록시에게 보낼 때)
                               0: 경로만 (원서버에게 보낼 때) */
    const char *ims;        /* NULL이 아니면 "If-Modified-Since: ims"를 붙인 조건부 요청 */
    const char *via_name;   /* Via 헤더에 덧붙일 이 노드의 이름 (NULL이면 Via를 손대지 않음) */
};

/* 위로 보낼 HTTP/1.0 요청을 out에 만든다. 반환: 길이, 공간 부족 -1
 *  - Connection: close 로 요청한다 (응답 끝 = 연결 종료)
 *  - hop-by-hop 헤더와 클라이언트의 조건부 헤더(If-Modified-Since, If-None-Match)는 뺀다
 *    (프록시가 캐시에 저장할 "완전한 200 응답"을 받기 위해서)
 *  - Via: 기존 값 뒤에 "받은 버전 노드이름"을 덧붙인다 (RFC 9110 7.6.3) */
int http_build_upstream_request(const struct http_request *req, const struct upstream_opts *o,
                                char *out, size_t cap);

/* 원서버 응답 헤더에서 캐시 판단에 필요한 정보 */
struct http_response_info {
    int       status;             /* 200, 304 ... */
    size_t    head_len;           /* 상태 줄 + 헤더 + 빈 줄의 길이 */
    long long content_length;     /* Content-Length 값 (없으면 -1) */
    int       no_store;           /* Cache-Control에 no-store 또는 private가 있으면 1 */
    char      last_modified[64];  /* Last-Modified 값 (없으면 빈 문자열) */
};

/* buf[0..len)에 들어 있는 응답의 헤더를 해석한다. 헤더 끝이 없거나 형식이 틀리면 -1 */
int http_parse_response_head(const char *buf, size_t len, struct http_response_info *ri);

/* 상태 코드에 맞는 오류 응답(HTML)을 보낸다 */
void http_send_error(int fd, int status, const char *detail);

/* 상태 코드의 이유 문구 (예: 502 → "Bad Gateway") */
const char *http_reason(int status);

/* 응답 첫 줄("HTTP/1.x 200 ...")에서 상태 코드를 꺼낸다. 실패하면 0 */
int http_parse_status(const char *buf, size_t len);

#endif /* HTTP_H */

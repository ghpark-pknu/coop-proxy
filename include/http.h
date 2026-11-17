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

/* 원서버에 보낼 HTTP/1.0 요청을 out에 만든다. 반환: 길이, 공간 부족 -1 */
int http_build_upstream_request(const struct http_request *req, char *out, size_t cap);

/* 상태 코드에 맞는 오류 응답(HTML)을 보낸다 */
void http_send_error(int fd, int status, const char *detail);

/* 상태 코드의 이유 문구 (예: 502 → "Bad Gateway") */
const char *http_reason(int status);

/* 응답 첫 줄("HTTP/1.x 200 ...")에서 상태 코드를 꺼낸다. 실패하면 0 */
int http_parse_status(const char *buf, size_t len);

#endif /* HTTP_H */

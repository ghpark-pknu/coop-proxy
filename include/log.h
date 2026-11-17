/*
 * log.h — 실행 기록
 *
 * 요청 하나가 끝날 때마다 한 줄씩 남긴다.
 *   2026-11-17 17:02:11.204 [node1] 127.0.0.1:53422 GET http://127.0.0.1:9000/a.html 200 1024B 3ms RELAY
 */
#ifndef LOG_H
#define LOG_H

/* 요청 하나의 처리 결과 */
struct req_log {
    const char *client;     /* "127.0.0.1:53422" */
    const char *method;     /* 해석 전에 실패했으면 "-" */
    const char *target;
    int         status;     /* 클라이언트가 받은 상태 코드 (모르면 0) */
    long long   bytes;      /* 원서버 → 클라이언트로 중계한 바이트 수 */
    long long   ms;         /* 요청 처리에 걸린 시간 */
    const char *result;     /* RELAY, ERROR, CLIENT_GONE ... (2단계부터 HIT/MISS 추가) */
};

void log_init(const char *node, int quiet);

/* 요청 기록 한 줄 (quiet 모드면 출력하지 않음) */
void log_request(const struct req_log *r);

/* 시작·종료·오류 같은 일반 메시지 (quiet 모드여도 출력) */
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* LOG_H */

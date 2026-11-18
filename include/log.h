/*
 * log.h — 실행 기록
 *
 * 요청 하나가 끝날 때마다 한 줄씩 남긴다. 맨 끝은 라우팅 정보 (클러스터 모드)
 *   ... [node1] 127.0.0.1:53422 GET http://127.0.0.1:9000/a.bin 200 7685B 3.1ms FORWARD →node2
 *   ... [node2] 127.0.0.1:41022 GET http://127.0.0.1:9000/a.bin 200 7685B 2.4ms MISS ←node1
 */
#ifndef LOG_H
#define LOG_H

#include "stats.h"

/* 요청 하나의 처리 결과 */
struct req_log {
    const char  *client;        /* "127.0.0.1:53422" */
    const char  *method;        /* 해석 전에 실패했으면 "-" */
    const char  *target;
    int          status;        /* 클라이언트가 받은 상태 코드 (모르면 0) */
    long long    bytes;         /* 클라이언트에 보낸 바이트 수 */
    long long    us;            /* 요청 처리에 걸린 시간 (마이크로초) */
    enum outcome outcome;       /* HIT, MISS, FORWARD ... */
    const char  *note;          /* 덧붙일 설명 (예: CLIENT_GONE). 없으면 NULL */
    int          origin_reqs;   /* 이 요청 때문에 원서버에 보낸 요청 수 */
    int          peer_failures; /* 연결하지 못한 다른 노드 수 */
    int          from_peer;     /* 다른 노드가 넘겨준 요청이면 1 */
    char         route[128];    /* 라우팅 설명: "→node2", "←node1", "대체 (node2 연결 불가)" */
};

void log_init(const char *node, int quiet);

/* 요청 기록 한 줄 (quiet 모드면 출력하지 않음) */
void log_request(const struct req_log *r);

/* 시작·종료·오류 같은 일반 메시지 (quiet 모드여도 출력) */
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* LOG_H */

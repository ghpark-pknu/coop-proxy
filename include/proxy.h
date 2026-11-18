/*
 * proxy.h — 요청 처리 흐름 (작업 스레드가 실행하는 부분)
 */
#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>

/* 실행 설정. 프로그램 시작 시 한 번 정하고, 작업 스레드들은 읽기만 한다 */
struct proxy_config {
    int    port;                  /* 클라이언트를 기다릴 포트 */
    char   node[32];              /* 노드 이름 (기록·X-Cache 헤더용, 3단계부터 클러스터에서 사용) */
    int    connect_timeout_ms;    /* 원서버 연결 제한 시간 */
    int    io_timeout_ms;         /* 송수신 제한 시간 (이 시간 동안 데이터가 안 오면 포기) */
    int    quiet;                 /* 1이면 요청 기록을 출력하지 않음 (부하 실험용) */
    size_t cache_capacity;        /* 캐시 전체 용량(바이트). 0이면 캐시를 쓰지 않음 */
    size_t max_object;            /* 캐시에 저장할 객체 하나의 상한(바이트) */
    int    cache_shards;          /* 캐시 샤드 수: 1 = 전역 mutex, 2 이상 = 샤드마다 mutex */
    int    validate_sec;          /* 검증 주기: 저장 후 이 시간이 지나면 원서버에 확인 */
    char   blocklist[256];        /* 차단 목록 파일 경로 (없으면 빈 문자열) */
    char   cluster_conf[256];     /* 노드 목록 파일 경로 (없으면 독립 프록시) */
    char   dashboard_path[1024];  /* 관제 화면 파일 (web/dashboard.html). 못 찾으면 빈 문자열 */
    int    port_given;            /* -p를 직접 지정했는가 (클러스터 모드에서 포트 확인용) */
    int    peer_retry_ms;         /* 연결에 실패한 노드를 다시 시도하기까지 건너뛰는 시간 */
};

/* 작업 스레드에 넘기는 연결 정보 (메인 스레드가 malloc, 작업 스레드가 free) */
struct client_conn {
    int  fd;
    char addr[64];                /* "127.0.0.1:53422" */
};

/* 설정 등록과 캐시 생성. 작업 스레드를 만들기 전에 한 번 호출한다. 성공 0 */
int  proxy_init(const struct proxy_config *cfg);

/* 작업 스레드의 시작 함수. arg는 struct client_conn* */
void *proxy_handle_connection(void *arg);

/* 진행 중인 연결 수 관리: 메인 스레드는 스레드를 만들기 직전에 opened(),
 * 스레드 생성에 실패하면 aborted()를 부른다. 작업 스레드는 끝날 때 스스로 줄인다 */
void proxy_conn_opened(void);
void proxy_conn_aborted(void);
int  proxy_active_conns(void);

/* 통계 요약을 기록으로 출력 */
void proxy_report(void);

/* ---- 관제(control.c)가 읽는 상태 ---- */
struct cache_usage;
const struct proxy_config *proxy_config(void);       /* 실행 설정 (읽기 전용) */
long long proxy_uptime_ms(void);                     /* 시작 후 지난 시간 */
int  proxy_cache_usage(struct cache_usage *u);       /* 캐시를 쓰면 1과 사용량, 끄면 0 */
struct cache;
struct cache *proxy_cache(void);                     /* 캐시 (끄면 NULL). 샤드별 조회용 */

/* 캐시 해제. 진행 중인 연결이 없을 때만 호출해야 한다 */
void proxy_cleanup(void);

#endif /* PROXY_H */

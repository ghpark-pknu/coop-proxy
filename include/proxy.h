/*
 * proxy.h — 요청 처리 흐름 (작업 스레드가 실행하는 부분)
 */
#ifndef PROXY_H
#define PROXY_H

/* 실행 설정. 프로그램 시작 시 한 번 정하고, 작업 스레드들은 읽기만 한다 */
struct proxy_config {
    int  port;                  /* 클라이언트를 기다릴 포트 */
    char node[32];              /* 노드 이름 (기록용, 3단계부터 클러스터에서 사용) */
    int  connect_timeout_ms;    /* 원서버 연결 제한 시간 */
    int  io_timeout_ms;         /* 송수신 제한 시간 (이 시간 동안 데이터가 안 오면 포기) */
    int  quiet;                 /* 1이면 요청 기록을 출력하지 않음 (부하 실험용) */
};

/* 작업 스레드에 넘기는 연결 정보 (메인 스레드가 malloc, 작업 스레드가 free) */
struct client_conn {
    int  fd;
    char addr[64];              /* "127.0.0.1:53422" */
};

/* 설정 등록. 작업 스레드를 만들기 전에 한 번 호출한다 */
void proxy_init(const struct proxy_config *cfg);

/* 작업 스레드의 시작 함수. arg는 struct client_conn* */
void *proxy_handle_connection(void *arg);

#endif /* PROXY_H */

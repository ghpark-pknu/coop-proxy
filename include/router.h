/*
 * router.h — CDN 라우터: 랑데부 해싱으로 URL마다 담당 노드를 정한다
 *
 * 랑데부 해싱 (Rendezvous Hashing, HRW: Highest Random Weight)
 *   URL 하나에 대해 노드마다 점수 = 해시(URL, 노드 이름)를 매기고,
 *   점수가 가장 높은 노드가 담당이다. 점수 순서가 곧 대체 순서다.
 *     1순위 = 담당 노드, 2순위 = 담당 노드에 장애가 났을 때 대신 맡는 노드
 *   모든 노드가 같은 노드 목록과 같은 함수를 쓰므로, 서로 묻지 않고도
 *   어느 노드에서 계산하든 같은 답이 나온다.
 *
 * 노드 목록 파일 (conf/cluster.conf): 한 줄에 "이름 호스트 포트"
 *   점수는 이름으로 계산한다. 노드의 주소가 바뀌어도 이름이 같으면 담당은 그대로다.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>
#include <stdint.h>

struct http_request;

#define ROUTER_MAX_NODES 16
#define ROUTER_NAME_MAX  32     /* 이름 최대 길이 + '\0' */
#define ROUTER_HOST_MAX  64

struct node {
    char     name[ROUTER_NAME_MAX];
    char     host[ROUTER_HOST_MAX];
    int      port;
    uint64_t seed;              /* 이름에서 만든 64비트 값 (점수 계산에 사용) */
};

/* ---- 랑데부 해싱: 전역 상태를 쓰지 않는 순수 함수 (단위 테스트에서 직접 사용) ---- */

uint64_t rendezvous_key(const char *key);                   /* URL → 64비트 */
uint64_t rendezvous_seed(const char *node_name);            /* 노드 이름 → 64비트 */
uint64_t rendezvous_score(uint64_t seed, uint64_t key_hash);

/* seeds[0..n)의 노드에 key의 점수를 매겨, 점수가 높은 순서로 노드 번호를 order에 채운다.
 * 점수가 같으면 번호가 작은 노드가 앞. scores가 NULL이 아니면 순서대로 점수도 채운다 */
void rendezvous_rank(const uint64_t *seeds, int n, const char *key,
                     int *order, uint64_t *scores);

/* ---- 클러스터 설정 ---- */

/* 노드 목록 파일을 읽는다. self_name이 NULL이 아니면 목록에 있어야 한다.
 * 성공 0, 실패 -1 (err에 이유) */
int  router_load(const char *path, const char *self_name, char *err, size_t errcap);

int  router_enabled(void);                  /* 노드 목록을 읽었는가 (클러스터 모드) */
int  router_count(void);
int  router_self(void);                     /* 이 노드의 번호 (없으면 -1) */
const struct node *router_node(int i);
int  router_find(const char *name);         /* 이름 → 번호 (대소문자 무시), 없으면 -1 */

/* key의 노드 순위를 order에 채우고 노드 수를 반환한다 */
int  router_rank(const char *key, int *order, uint64_t *scores);

/* ---- 루프 방지 ---- */

/* 요청의 Via 헤더에 클러스터 노드 이름이 있으면 그 노드 번호(가장 최근에 거친 노드),
 * 없으면 -1. 다른 노드가 넘겨준 요청은 다시 넘기지 않는다 */
int  router_via_peer(const struct http_request *req);

/* ---- 장애 표시 (노드마다 원자 변수 하나: 락 없이 여러 스레드가 함께 씀) ---- */

/* 지금 이 노드에 요청을 보내 봐도 되는가 (장애 표시가 없거나 재시도 시각이 지남) */
int  router_available(int i, long long now_ms);

/* 연결 실패: retry_ms 동안 건너뛰도록 표시. 정상이던 노드를 새로 표시했으면 1 */
int  router_mark_down(int i, long long now_ms, int retry_ms);

/* 연결 성공: 장애 표시를 지운다. 장애로 표시돼 있던 노드였으면 1 (복구) */
int  router_mark_up(int i);

#endif /* ROUTER_H */

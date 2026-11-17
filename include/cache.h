/*
 * cache.h — 용량 제한 LRU 캐시 (해시 테이블 + 이중 연결 리스트 + mutex)
 *
 *   해시 테이블 : URL → 객체를 O(1)에 찾는다
 *   이중 연결 리스트 : 최근에 쓴 순서. 맨 앞 = 가장 최근, 맨 뒤 = 가장 오래 안 쓴 객체
 *   용량 제한 : 저장한 바이트 합이 capacity를 넘으면 맨 뒤부터 내보낸다 (LRU 교체)
 *   mutex : 여러 작업 스레드가 동시에 접근하므로 모든 연산을 락 안에서 한다
 *
 * 참조 카운트(refcount)
 *   cache_lookup()으로 받은 객체는 클라이언트에 다 보낼 때까지 읽어야 한다.
 *   그동안 다른 스레드가 같은 객체를 교체·삭제해도 메모리가 해제되지 않도록,
 *   "지금 이 객체를 읽는 중인 스레드 수"를 센다. 다 쓰면 반드시 cache_release()로 반납한다.
 *   네트워크 전송은 락 밖에서 하므로, 느린 클라이언트가 캐시 전체를 붙잡지 않는다.
 */
#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

/* 캐시에 저장된 응답 하나 */
struct cache_obj {
    /* ── 다른 모듈이 읽어도 되는 필드 (저장 후 바뀌지 않음) ── */
    char   *url;                /* 키 */
    char   *data;               /* 원서버 응답 원문: 상태 줄 + 헤더 + 본문 */
    size_t  len;                /* data의 바이트 수 */
    char    last_modified[64];  /* Last-Modified 헤더 값 (없으면 빈 문자열) */

    /* ── cache.c 내부용 (락을 잡고만 접근) ── */
    long long          stored_at_ms;   /* 저장(또는 마지막 검증) 시각 */
    int                refcount;       /* 읽는 중인 스레드 수 */
    int                in_cache;       /* 아직 테이블·리스트에 들어 있는가 */
    struct cache_obj  *hnext;          /* 같은 버킷의 다음 객체 (체이닝) */
    struct cache_obj  *prev, *next;    /* LRU 리스트 */
};

/* 캐시 상태 (통계 출력용) */
struct cache_usage {
    size_t        count;        /* 저장된 객체 수 */
    size_t        bytes;        /* 저장된 바이트 합 */
    size_t        capacity;     /* 용량 */
    unsigned long evictions;    /* 용량 때문에 내보낸 횟수 */
};

struct cache;   /* 내부 구조는 cache.c에만 있다 (불투명 타입) */

/* capacity: 전체 용량(바이트), max_object: 객체 하나의 상한(바이트) */
struct cache *cache_create(size_t capacity, size_t max_object);
void          cache_destroy(struct cache *c);

/* url을 찾는다. 있으면 참조를 하나 늘려 반환하고, 마지막 저장·검증 후
 * ttl_ms가 지났으면 *expired = 1. 없으면 NULL */
struct cache_obj *cache_lookup(struct cache *c, const char *url,
                               long long now_ms, long long ttl_ms, int *expired);

/* cache_lookup으로 받은 객체를 반납한다 */
void cache_release(struct cache *c, struct cache_obj *o);

/* 응답을 저장한다 (같은 url이 있으면 교체). data의 소유권을 넘겨받는다.
 * 저장했으면 1, 너무 커서 저장하지 않았으면 0 (이때도 data는 해제된다) */
int cache_insert(struct cache *c, const char *url, char *data, size_t len,
                 const char *last_modified, long long now_ms);

/* 304 Not Modified로 검증이 끝난 객체의 저장 시각을 갱신한다 */
void cache_refresh(struct cache *c, struct cache_obj *o, long long now_ms);

size_t cache_max_object(const struct cache *c);
void   cache_get_usage(struct cache *c, struct cache_usage *u);

#endif /* CACHE_H */

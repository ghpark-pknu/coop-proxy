/*
 * cache.c — 용량 제한 LRU 캐시 (샤드 N개)
 *
 * 규칙 세 가지
 *   1) 객체에 대한 모든 연산은 그 객체가 속한 샤드의 락을 잡은 상태에서 한다.
 *   2) 락 안에서는 메모리 작업만 한다. 소켓 송수신 같은 느린 일은 절대 하지 않는다.
 *      URL 해시 계산, 객체 메모리 할당도 락 밖에서 미리 한다 (락을 잡는 시간을 줄인다).
 *   3) 한 번에 샤드 락 하나만 잡는다 (여러 샤드를 함께 잡는 연산이 없으므로 교착 상태가 없다).
 *      통계 조회(cache_get_usage)도 샤드를 하나씩 잠그고 풀며 더한다.
 */
#include "cache.h"
#include "hash.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBUCKETS 4096   /* 샤드 하나의 해시 버킷 수. 2의 거듭제곱이라 % 대신 & 로 나머지를 구한다 */

/* 샤드 하나 = 2단계의 캐시 한 벌.
 * aligned(64): 샤드마다 CPU 캐시 줄(64바이트) 경계에서 시작하게 한다. 그러지 않으면
 * 앞 샤드의 끝(통계 필드)과 다음 샤드의 락이 같은 캐시 줄에 놓일 수 있고, 서로 다른 락을
 * 쓰는 두 코어가 같은 캐시 줄을 번갈아 빼앗는 "거짓 공유(false sharing)"가 생긴다 */
struct cache_shard {
    pthread_mutex_t    lock;
    struct lock_stats  ls;              /* 이 샤드 락의 경합 기록 (lock이 보호) */
    struct cache_obj  *head;            /* 가장 최근에 쓴 객체 */
    struct cache_obj  *tail;            /* 가장 오래 안 쓴 객체 → 교체 대상 */
    size_t             count;
    size_t             bytes;
    size_t             capacity;
    unsigned long      evictions;
    struct cache_obj  *buckets[NBUCKETS];
} __attribute__((aligned(64)));

struct cache {
    int                 nshards;
    size_t              max_object;
    struct cache_shard *shards;         /* nshards개 (64바이트 경계에 정렬된 배열) */
};

/* 샤드 고르기.
 * 버킷 번호는 해시의 아래 12비트(h & 4095)로 고른다. 샤드까지 같은 아래 비트로 고르면
 * (예: h % 8) 한 샤드에 들어간 URL들은 아래 3비트가 모두 같아서, 그 샤드의 버킷 4096개 중
 * 512개만 쓰이고 체인이 8배 길어진다. 그래서 해시를 한 번 더 섞은 값으로 샤드를 고른다.
 * (mix64는 입력의 모든 비트가 출력의 모든 비트에 영향을 주는 일대일 함수다)
 * 샤드가 1개여도 같은 계산을 한다 → 두 전략의 차이가 "락의 개수"뿐이게 */
static struct cache_shard *shard_for(const struct cache *c, uint64_t h)
{
    return &c->shards[hash_mix64(h) % (uint64_t)c->nshards];
}

/* 객체가 차지하는 바이트: 응답 크기 + URL 길이 */
static size_t obj_cost(const struct cache_obj *o)
{
    return o->len + strlen(o->url) + 1;
}

static void obj_free(struct cache_obj *o)
{
    free(o->url);
    free(o->data);
    free(o);
}

/* ---- LRU 리스트 조작 (샤드 락 안에서만 호출) ---- */

static void list_unlink(struct cache_shard *s, struct cache_obj *o)
{
    if (o->prev) o->prev->next = o->next; else s->head = o->next;
    if (o->next) o->next->prev = o->prev; else s->tail = o->prev;
    o->prev = o->next = NULL;
}

static void list_push_front(struct cache_shard *s, struct cache_obj *o)
{
    o->prev = NULL;
    o->next = s->head;
    if (s->head) s->head->prev = o; else s->tail = o;
    s->head = o;
}

/* ---- 해시 테이블 조작 (샤드 락 안에서만 호출. 해시는 락 밖에서 계산해 넘긴다) ---- */

static struct cache_obj *table_find(struct cache_shard *s, const char *url, uint64_t h)
{
    struct cache_obj *o = s->buckets[h & (NBUCKETS - 1)];
    for (; o != NULL; o = o->hnext)
        if (o->hash == h && strcmp(o->url, url) == 0)
            return o;
    return NULL;
}

static void table_remove(struct cache_shard *s, struct cache_obj *o)
{
    struct cache_obj **pp = &s->buckets[o->hash & (NBUCKETS - 1)];
    while (*pp != o)
        pp = &(*pp)->hnext;
    *pp = o->hnext;
    o->hnext = NULL;
}

/* 객체를 테이블과 리스트에서 뺀다. 읽는 스레드가 없으면 바로 해제하고,
 * 있으면 마지막 스레드가 cache_release()할 때 해제된다 */
static void detach(struct cache_shard *s, struct cache_obj *o)
{
    table_remove(s, o);
    list_unlink(s, o);
    o->in_cache = 0;
    s->count--;
    s->bytes -= obj_cost(o);
    if (o->refcount == 0)
        obj_free(o);
}

/* ---- 공개 함수 ---- */

struct cache *cache_create(size_t capacity, size_t max_object, int nshards)
{
    if (nshards < 1 || nshards > CACHE_MAX_SHARDS)
        return NULL;
    size_t per = capacity / (size_t)nshards;       /* 가장 작은 샤드의 용량 */
    size_t rem = capacity % (size_t)nshards;       /* 남는 바이트: 앞 샤드부터 1바이트씩 */
    if (per == 0)
        return NULL;

    struct cache *c = calloc(1, sizeof *c);
    if (c == NULL)
        return NULL;
    /* 정렬된 배열: 크기가 정렬 단위(64)의 배수여야 한다. sizeof(struct cache_shard)는
     * aligned(64) 때문에 이미 64의 배수다 */
    c->shards = aligned_alloc(64, (size_t)nshards * sizeof(struct cache_shard));
    if (c->shards == NULL) {
        free(c);
        return NULL;
    }
    memset(c->shards, 0, (size_t)nshards * sizeof(struct cache_shard));
    for (int i = 0; i < nshards; i++) {
        pthread_mutex_init(&c->shards[i].lock, NULL);
        c->shards[i].capacity = per + ((size_t)i < rem ? 1 : 0);
    }
    c->nshards    = nshards;
    c->max_object = max_object < per ? max_object : per;   /* 객체는 샤드 하나에 들어가야 한다 */
    return c;
}

void cache_destroy(struct cache *c)
{
    if (c == NULL)
        return;
    for (int i = 0; i < c->nshards; i++) {
        struct cache_shard *s = &c->shards[i];
        struct cache_obj *o = s->head;
        while (o != NULL) {
            struct cache_obj *next = o->next;
            obj_free(o);
            o = next;
        }
        pthread_mutex_destroy(&s->lock);
    }
    free(c->shards);
    free(c);
}

struct cache_obj *cache_lookup(struct cache *c, const char *url,
                               long long now_ms, long long ttl_ms, int *expired)
{
    uint64_t h = hash_fnv1a(url);                   /* 락 밖에서 */
    struct cache_shard *s = shard_for(c, h);

    lockstat_lock(&s->lock, &s->ls);
    struct cache_obj *o = table_find(s, url, h);
    if (o != NULL) {
        o->refcount++;                  /* 반납할 때까지 해제되지 않게 */
        list_unlink(s, o);              /* 방금 썼으므로 맨 앞으로 */
        list_push_front(s, o);
        *expired = (now_ms - o->stored_at_ms) >= ttl_ms;
    }
    pthread_mutex_unlock(&s->lock);
    return o;
}

void cache_release(struct cache *c, struct cache_obj *o)
{
    (void)c;
    struct cache_shard *s = o->shard;
    lockstat_lock(&s->lock, &s->ls);
    o->refcount--;
    /* 읽는 동안 교체·삭제된 객체라면 마지막으로 반납하는 스레드가 해제한다 */
    int free_now = (o->refcount == 0 && !o->in_cache);
    pthread_mutex_unlock(&s->lock);
    if (free_now)
        obj_free(o);
}

int cache_insert(struct cache *c, const char *url, char *data, size_t len,
                 const char *last_modified, long long now_ms)
{
    if (len > c->max_object) {
        free(data);
        return 0;
    }

    /* 객체는 락 밖에서 미리 만들어 둔다 (malloc은 느릴 수 있으므로 락을 짧게) */
    struct cache_obj *o = calloc(1, sizeof *o);
    char *ucopy = strdup(url);
    if (o == NULL || ucopy == NULL) {
        free(o);
        free(ucopy);
        free(data);
        return 0;
    }
    o->url   = ucopy;
    o->data  = data;
    o->len   = len;
    o->hash  = hash_fnv1a(url);
    o->shard = shard_for(c, o->hash);
    snprintf(o->last_modified, sizeof o->last_modified, "%s",
             last_modified ? last_modified : "");
    o->stored_at_ms = now_ms;
    o->in_cache     = 1;

    struct cache_shard *s = o->shard;
    lockstat_lock(&s->lock, &s->ls);

    struct cache_obj *old = table_find(s, url, o->hash);
    if (old != NULL)
        detach(s, old);                 /* 같은 URL은 새 응답으로 교체 */

    size_t slot = o->hash & (NBUCKETS - 1);
    o->hnext = s->buckets[slot];
    s->buckets[slot] = o;
    list_push_front(s, o);
    s->count++;
    s->bytes += obj_cost(o);

    /* 이 샤드의 용량을 넘었으면 이 샤드에서 가장 오래 안 쓴 객체(맨 뒤)부터 내보낸다 */
    while (s->bytes > s->capacity && s->tail != NULL && s->tail != o) {
        detach(s, s->tail);
        s->evictions++;
    }

    pthread_mutex_unlock(&s->lock);
    return 1;
}

void cache_refresh(struct cache *c, struct cache_obj *o, long long now_ms)
{
    (void)c;
    struct cache_shard *s = o->shard;
    lockstat_lock(&s->lock, &s->ls);
    if (o->in_cache)
        o->stored_at_ms = now_ms;
    pthread_mutex_unlock(&s->lock);
}

/* 아래 세 값은 생성 후 바뀌지 않으므로 락이 필요 없다 */
size_t cache_max_object(const struct cache *c) { return c->max_object; }
int    cache_shards(const struct cache *c)     { return c->nshards; }

int cache_shard_of(const struct cache *c, const char *url)
{
    return (int)(shard_for(c, hash_fnv1a(url)) - c->shards);
}

/* 샤드 하나의 상태 (조회는 경합 기록에 넣지 않으려고 lockstat_lock 대신 그냥 잠근다) */
void cache_get_shard_usage(struct cache *c, int i, struct cache_usage *u)
{
    struct cache_shard *s = &c->shards[i];
    pthread_mutex_lock(&s->lock);
    u->count     = s->count;
    u->bytes     = s->bytes;
    u->capacity  = s->capacity;
    u->evictions = s->evictions;
    u->lock      = s->ls;
    pthread_mutex_unlock(&s->lock);
    u->shards = 1;
}

/* 모든 샤드의 합. 샤드를 하나씩 잠그고 풀며 더하므로, 그 사이에 다른 샤드가 바뀔 수 있다
 * (전체를 한 순간에 찍은 값은 아니다). 관제·통계용으로는 충분하다 */
void cache_get_usage(struct cache *c, struct cache_usage *u)
{
    memset(u, 0, sizeof *u);
    for (int i = 0; i < c->nshards; i++) {
        struct cache_usage one;
        cache_get_shard_usage(c, i, &one);
        u->count     += one.count;
        u->bytes     += one.bytes;
        u->capacity  += one.capacity;
        u->evictions += one.evictions;
        lockstat_add(&u->lock, &one.lock);
    }
    u->shards = c->nshards;
}

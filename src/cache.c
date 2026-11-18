/*
 * cache.c — 용량 제한 LRU 캐시
 *
 * 규칙 두 가지
 *   1) 모든 연산은 c->lock을 잡은 상태에서 한다.
 *   2) 락 안에서는 메모리 작업만 한다. 소켓 송수신 같은 느린 일은 절대 하지 않는다.
 */
#include "cache.h"
#include "hash.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBUCKETS 4096   /* 해시 버킷 수. 2의 거듭제곱이라 % 대신 & 로 나머지를 구한다 */

struct cache {
    pthread_mutex_t    lock;
    struct cache_obj  *buckets[NBUCKETS];
    struct cache_obj  *head;        /* 가장 최근에 쓴 객체 */
    struct cache_obj  *tail;        /* 가장 오래 안 쓴 객체 → 교체 대상 */
    size_t             count;
    size_t             bytes;
    size_t             capacity;
    size_t             max_object;
    unsigned long      evictions;
};

/* 버킷 번호: URL의 FNV-1a 해시 (hash.h) */
static uint64_t hash_str(const char *s)
{
    return hash_fnv1a(s);
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

/* ---- LRU 리스트 조작 (락 안에서만 호출) ---- */

static void list_unlink(struct cache *c, struct cache_obj *o)
{
    if (o->prev) o->prev->next = o->next; else c->head = o->next;
    if (o->next) o->next->prev = o->prev; else c->tail = o->prev;
    o->prev = o->next = NULL;
}

static void list_push_front(struct cache *c, struct cache_obj *o)
{
    o->prev = NULL;
    o->next = c->head;
    if (c->head) c->head->prev = o; else c->tail = o;
    c->head = o;
}

/* ---- 해시 테이블 조작 (락 안에서만 호출) ---- */

static struct cache_obj *table_find(struct cache *c, const char *url)
{
    struct cache_obj *o = c->buckets[hash_str(url) & (NBUCKETS - 1)];
    for (; o != NULL; o = o->hnext)
        if (strcmp(o->url, url) == 0)
            return o;
    return NULL;
}

static void table_remove(struct cache *c, struct cache_obj *o)
{
    struct cache_obj **pp = &c->buckets[hash_str(o->url) & (NBUCKETS - 1)];
    while (*pp != o)
        pp = &(*pp)->hnext;
    *pp = o->hnext;
    o->hnext = NULL;
}

/* 객체를 테이블과 리스트에서 뺀다. 읽는 스레드가 없으면 바로 해제하고,
 * 있으면 마지막 스레드가 cache_release()할 때 해제된다 */
static void detach(struct cache *c, struct cache_obj *o)
{
    table_remove(c, o);
    list_unlink(c, o);
    o->in_cache = 0;
    c->count--;
    c->bytes -= obj_cost(o);
    if (o->refcount == 0)
        obj_free(o);
}

/* ---- 공개 함수 ---- */

struct cache *cache_create(size_t capacity, size_t max_object)
{
    struct cache *c = calloc(1, sizeof *c);
    if (c == NULL)
        return NULL;
    pthread_mutex_init(&c->lock, NULL);
    c->capacity   = capacity;
    c->max_object = max_object < capacity ? max_object : capacity;
    return c;
}

void cache_destroy(struct cache *c)
{
    if (c == NULL)
        return;
    struct cache_obj *o = c->head;
    while (o != NULL) {
        struct cache_obj *next = o->next;
        obj_free(o);
        o = next;
    }
    pthread_mutex_destroy(&c->lock);
    free(c);
}

struct cache_obj *cache_lookup(struct cache *c, const char *url,
                               long long now_ms, long long ttl_ms, int *expired)
{
    pthread_mutex_lock(&c->lock);
    struct cache_obj *o = table_find(c, url);
    if (o != NULL) {
        o->refcount++;                  /* 반납할 때까지 해제되지 않게 */
        list_unlink(c, o);              /* 방금 썼으므로 맨 앞으로 */
        list_push_front(c, o);
        *expired = (now_ms - o->stored_at_ms) >= ttl_ms;
    }
    pthread_mutex_unlock(&c->lock);
    return o;
}

void cache_release(struct cache *c, struct cache_obj *o)
{
    pthread_mutex_lock(&c->lock);
    o->refcount--;
    /* 읽는 동안 교체·삭제된 객체라면 마지막으로 반납하는 스레드가 해제한다 */
    int free_now = (o->refcount == 0 && !o->in_cache);
    pthread_mutex_unlock(&c->lock);
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
    o->url  = ucopy;
    o->data = data;
    o->len  = len;
    snprintf(o->last_modified, sizeof o->last_modified, "%s",
             last_modified ? last_modified : "");
    o->stored_at_ms = now_ms;
    o->in_cache     = 1;

    pthread_mutex_lock(&c->lock);

    struct cache_obj *old = table_find(c, url);
    if (old != NULL)
        detach(c, old);                 /* 같은 URL은 새 응답으로 교체 */

    size_t slot = hash_str(url) & (NBUCKETS - 1);
    o->hnext = c->buckets[slot];
    c->buckets[slot] = o;
    list_push_front(c, o);
    c->count++;
    c->bytes += obj_cost(o);

    /* 용량을 넘었으면 가장 오래 안 쓴 객체(맨 뒤)부터 내보낸다 */
    while (c->bytes > c->capacity && c->tail != NULL && c->tail != o) {
        detach(c, c->tail);
        c->evictions++;
    }

    pthread_mutex_unlock(&c->lock);
    return 1;
}

void cache_refresh(struct cache *c, struct cache_obj *o, long long now_ms)
{
    pthread_mutex_lock(&c->lock);
    if (o->in_cache)
        o->stored_at_ms = now_ms;
    pthread_mutex_unlock(&c->lock);
}

size_t cache_max_object(const struct cache *c)
{
    return c->max_object;       /* 생성 후 바뀌지 않으므로 락 불필요 */
}

void cache_get_usage(struct cache *c, struct cache_usage *u)
{
    pthread_mutex_lock(&c->lock);
    u->count     = c->count;
    u->bytes     = c->bytes;
    u->capacity  = c->capacity;
    u->evictions = c->evictions;
    pthread_mutex_unlock(&c->lock);
}

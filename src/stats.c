/*
 * stats.c — 통계 수집기
 */
#include "stats.h"

#include <pthread.h>
#include <string.h>

/* 히스토그램 구간 경계 (마이크로초). 마지막 구간(26번)은 5초 초과 */
const long long STATS_BOUNDS_US[STATS_NBUCKETS - 1] = {
    50, 100, 200, 300, 500, 750,                        /* 0.05 ~ 0.75ms: 캐시 HIT */
    1000, 1500, 2000, 3000, 5000, 7500,                 /* 1 ~ 7.5ms: MISS, 전달 */
    10000, 15000, 20000, 30000, 50000, 75000,           /* 10 ~ 75ms */
    100000, 150000, 200000, 300000, 500000,             /* 0.1 ~ 0.5초 */
    1000000, 2000000, 5000000,                          /* 1 ~ 5초: 시간 초과 근처 */
};

static pthread_mutex_t       g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stats_snapshot g_stats;

static const char *const NAMES[OUT_COUNT] = {
    "HIT", "VALIDATED", "STALE", "MISS", "REFRESH", "FORWARD", "BLOCKED", "ERROR",
};

const char *outcome_name(enum outcome o)
{
    return ((unsigned)o < OUT_COUNT) ? NAMES[o] : "?";
}

int stats_bucket(long long us)
{
    for (int i = 0; i < STATS_NBUCKETS - 1; i++)
        if (us <= STATS_BOUNDS_US[i])
            return i;
    return STATS_NBUCKETS - 1;
}

void stats_record(const struct stats_event *e)
{
    int bucket = stats_bucket(e->us);       /* 계산은 락 밖에서 → 락을 잡는 시간이 짧아진다 */

    pthread_mutex_lock(&g_lock);
    g_stats.count[e->outcome]++;
    g_stats.requests++;
    g_stats.origin_requests += (unsigned long)e->origin_requests;
    g_stats.peer_failures   += (unsigned long)e->peer_failures;
    g_stats.bytes           += (unsigned long long)e->bytes;

    struct latency_stats *l = e->from_peer ? &g_stats.peer : &g_stats.client;
    l->count++;
    l->sum_us += (unsigned long long)e->us;
    if (e->us > l->max_us)
        l->max_us = e->us;
    l->hist[bucket]++;
    pthread_mutex_unlock(&g_lock);
}

void stats_get(struct stats_snapshot *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_stats;             /* 락 안에서 통째로 복사 → 서로 맞는 값들의 묶음 */
    pthread_mutex_unlock(&g_lock);
}

double stats_hit_ratio(const struct stats_snapshot *s)
{
    unsigned long hits  = s->count[OUT_HIT] + s->count[OUT_VALIDATED] + s->count[OUT_STALE];
    unsigned long total = hits + s->count[OUT_MISS] + s->count[OUT_REFRESH];
    return total ? (double)hits / (double)total : 0.0;
}

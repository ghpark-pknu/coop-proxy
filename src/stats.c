/*
 * stats.c — 통계 수집기
 */
#include "stats.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t       g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stats_snapshot g_stats;

static const char *const NAMES[OUT_COUNT] = {
    "HIT", "VALIDATED", "STALE", "MISS", "REFRESH", "FORWARD", "BLOCKED", "ERROR",
};

const char *outcome_name(enum outcome o)
{
    return ((unsigned)o < OUT_COUNT) ? NAMES[o] : "?";
}

void stats_record(const struct stats_event *e)
{
    pthread_mutex_lock(&g_lock);
    g_stats.count[e->outcome]++;
    g_stats.requests++;
    g_stats.origin_requests += (unsigned long)e->origin_requests;
    g_stats.peer_failures   += (unsigned long)e->peer_failures;
    g_stats.from_peer       += (unsigned long)(e->from_peer != 0);
    g_stats.bytes  += (unsigned long long)e->bytes;
    g_stats.sum_ms += (unsigned long long)e->ms;
    if (e->ms > g_stats.max_ms)
        g_stats.max_ms = e->ms;
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

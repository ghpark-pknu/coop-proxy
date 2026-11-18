/*
 * cache_test.c — 캐시 단위 테스트와 락 전략 마이크로 벤치마크 (5단계)
 *
 * 빌드: make ctest            (결과물: build/<모드>/cache_test)
 *
 *   ./build/debug/cache_test
 *       모든 검사를 실행한다. 하나라도 실패하면 종료 코드 1
 *       (make MODE=tsan ctest → ./build/tsan/cache_test 로 데이터 경쟁까지 검사)
 *
 *   ./build/release/cache_test bench [-t 스레드] [-d 초] [-k 키 수] [-z Zipf 지수] [-S 샤드 목록]
 *       네트워크 없이 캐시만 두드려서 샤드 수별 처리량과 락 경합을 잰다.
 *       스레드마다 Zipf 분포로 URL을 골라 cache_lookup → cache_release를 반복한다
 *       (프록시의 HIT 처리와 같은 락 사용: 요청 하나에 캐시 락 2번).
 *       기본: 스레드 = CPU 수, 2초, 키 200개, Zipf 0.8, 샤드 1,2,4,8,16
 */
#include "cache.h"
#include "hash.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s ", ok ? "\033[32m✓\033[0m" : "\033[31m✗\033[0m");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok)
        g_failures++;
}

static void url_of(char *buf, size_t cap, const char *kind, int i)
{
    snprintf(buf, cap, "http://127.0.0.1:9000/%s/%05d.bin", kind, i);
}

/* 내용을 검사할 수 있는 응답 데이터: "URL|" 뒤를 URL에서 정한 글자로 채운다 */
static char fill_char(const char *url)
{
    return (char)('a' + hash_fnv1a(url) % 26);
}

static char *make_data(const char *url, size_t len)
{
    size_t ulen = strlen(url);
    char *d = malloc(len);
    if (d == NULL) {
        perror("malloc");
        exit(2);
    }
    memset(d, fill_char(url), len);
    if (len > ulen) {
        memcpy(d, url, ulen);
        d[ulen] = '|';
    }
    return d;
}

/* 객체 내용이 url의 것인가 (다른 객체와 섞이거나 해제된 메모리를 읽으면 틀린다) */
static int data_ok(const struct cache_obj *o, const char *url)
{
    size_t ulen = strlen(url);
    if (o->len <= ulen || memcmp(o->data, url, ulen) != 0 || o->data[ulen] != '|')
        return 0;
    return o->data[o->len - 1] == fill_char(url);
}

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* 스레드마다 따로 쓰는 난수 (xorshift64*) */
static uint64_t rnd(uint64_t *s)
{
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 2685821657736338717ULL;
}

/* 샤드마다 "용량 이하이거나 객체가 1개뿐"이어야 한다.
 * (새로 넣은 객체 자신은 내보내지 않으므로, 큰 객체 하나가 용량을 조금 넘을 수는 있다) */
static int shards_within_capacity(struct cache *c)
{
    for (int i = 0; i < cache_shards(c); i++) {
        struct cache_usage u;
        cache_get_shard_usage(c, i, &u);
        if (u.bytes > u.capacity && u.count > 1)
            return 0;
    }
    return 1;
}

/* ================================================================== */
/* 검사                                                                 */
/* ================================================================== */

static void test_shard_bits(void)
{
    enum { N = 100000, S = 8, NB = 4096 };
    printf("[1] 샤드 고르기: URL %d개, 샤드 %d개\n", N, S);

    /* 설계 근거: 버킷은 해시의 아래 12비트로 고른다. 샤드를 같은 아래 비트로 고르면? */
    static unsigned char naive_used[NB], mixed_used[NB];
    int naive = 0, mixed = 0;
    char url[96];
    for (int i = 0; i < N; i++) {
        url_of(url, sizeof url, "bits", i);
        uint64_t h = hash_fnv1a(url);
        unsigned b = (unsigned)(h & (NB - 1));
        if (h % S == 0 && !naive_used[b]) {
            naive_used[b] = 1;
            naive++;
        }
        if (hash_mix64(h) % S == 0 && !mixed_used[b]) {
            mixed_used[b] = 1;
            mixed++;
        }
    }
    printf("      샤드 0이 쓰는 버킷 수 (버킷 %d개 중): h %% 8로 고르면 %d개, mix64(h) %% 8로 고르면 %d개\n",
           NB, naive, mixed);
    check(naive <= NB / S, "샤드를 버킷과 같은 아래 비트(h %% 8)로 고르면 버킷의 1/8(512개) 이하만 쓰임");
    check(mixed > NB * 9 / 10, "해시를 섞은 값(mix64)으로 고르면 버킷의 90%%를 넘게 씀 → cache.c의 방식");

    struct cache *c = cache_create(1 << 20, 1024, S);
    long cnt[S] = { 0 };
    int formula = 1;
    for (int i = 0; i < N; i++) {
        url_of(url, sizeof url, "bits", i);
        int sh = cache_shard_of(c, url);
        cnt[sh]++;
        if ((uint64_t)sh != hash_mix64(hash_fnv1a(url)) % S)
            formula = 0;
    }
    check(formula, "cache.c가 실제로 mix64(h) %% 샤드 수로 샤드를 고름 (URL %d개 모두)", N);
    int even = 1;
    printf("      샤드별 URL 수:");
    for (int i = 0; i < S; i++) {
        printf(" %ld", cnt[i]);
        if (cnt[i] < N / S * 95 / 100 || cnt[i] > N / S * 105 / 100)
            even = 0;
    }
    printf("\n");
    check(even, "cache_shard_of: 샤드마다 URL이 고르게 퍼짐 (평균의 ±5%% 이내)");
    cache_destroy(c);
}

static void test_create(void)
{
    printf("[2] 용량 나누기와 객체 상한\n");
    struct cache *c = cache_create(1000, 10000, 3);
    check(c != NULL && cache_shards(c) == 3, "용량 1000바이트, 샤드 3개로 생성");
    if (c == NULL)
        return;
    struct cache_usage u0, u1, u2, all;
    cache_get_shard_usage(c, 0, &u0);
    cache_get_shard_usage(c, 1, &u1);
    cache_get_shard_usage(c, 2, &u2);
    cache_get_usage(c, &all);
    check(u0.capacity == 334 && u1.capacity == 333 && u2.capacity == 333 && all.capacity == 1000,
          "샤드 용량 %zu·%zu·%zu, 합 %zu (나머지 1바이트는 앞 샤드에)",
          u0.capacity, u1.capacity, u2.capacity, all.capacity);
    check(cache_max_object(c) == 333, "객체 상한은 가장 작은 샤드의 용량(333)으로 줄어듦 → %zu",
          cache_max_object(c));
    cache_destroy(c);

    c = cache_create(1000, 10000, 1);
    check(c != NULL && cache_max_object(c) == 1000, "샤드 1개: 객체 상한 = 캐시 전체 용량 (2단계와 같음)");
    cache_destroy(c);

    check(cache_create(1000, 10, 0) == NULL, "샤드 0개 → 거부");
    check(cache_create(1000, 10, CACHE_MAX_SHARDS + 1) == NULL, "샤드 %d개 → 거부", CACHE_MAX_SHARDS + 1);
    check(cache_create(2, 1, 3) == NULL, "샤드 하나의 용량이 0 (용량 2, 샤드 3) → 거부");
}

static void test_shard_lru(void)
{
    printf("[3] 샤드별 LRU: 샤드 4개 × 100000바이트, 객체 40000바이트\n");
    struct cache *c = cache_create(400000, 50000, 4);
    char a[96], b[96], cc[96], x[96], u[96];

    /* 같은 샤드에 들어가는 URL 셋(a, b, c)과 다른 샤드의 URL 하나(x)를 찾는다 */
    url_of(a, sizeof a, "lru", 0);
    int k = cache_shard_of(c, a);
    int found = 0;
    b[0] = cc[0] = x[0] = '\0';
    for (int i = 1; i < 1000 && found < 3; i++) {
        url_of(u, sizeof u, "lru", i);
        int s = cache_shard_of(c, u);
        if (s == k && b[0] == '\0')       { memcpy(b, u, sizeof u); found++; }
        else if (s == k && cc[0] == '\0') { memcpy(cc, u, sizeof u); found++; }
        else if (s != k && x[0] == '\0')  { memcpy(x, u, sizeof u); found++; }
    }
    if (found < 3) {
        check(0, "시험용 URL을 찾지 못함");
        cache_destroy(c);
        return;
    }

    int exp;
    struct cache_obj *o;
    cache_insert(c, a, make_data(a, 40000), 40000, "", 0);
    cache_insert(c, b, make_data(b, 40000), 40000, "", 0);
    o = cache_lookup(c, a, 0, 1000, &exp);          /* a를 사용 → a가 b보다 최근 */
    if (o) cache_release(c, o);
    cache_insert(c, x, make_data(x, 40000), 40000, "", 0);     /* 다른 샤드 */
    struct cache_usage all;
    cache_get_usage(c, &all);
    check(all.evictions == 0, "a, b(샤드 %d)와 x(샤드 %d) 저장: 교체 없음", k, cache_shard_of(c, x));

    cache_insert(c, cc, make_data(cc, 40000), 40000, "", 0);   /* 샤드 k에 세 번째 → 넘침 */
    int has_a = (o = cache_lookup(c, a, 0, 1000, &exp)) != NULL;
    if (o) cache_release(c, o);
    int has_b = (o = cache_lookup(c, b, 0, 1000, &exp)) != NULL;
    if (o) cache_release(c, o);
    int has_c = (o = cache_lookup(c, cc, 0, 1000, &exp)) != NULL;
    if (o) cache_release(c, o);
    int has_x = (o = cache_lookup(c, x, 0, 1000, &exp)) != NULL;
    if (o) cache_release(c, o);
    cache_get_usage(c, &all);
    check(!has_b && has_a && has_c, "c 저장 → 같은 샤드에서 가장 오래 안 쓴 b가 나감 (a는 최근에 써서 남음)");
    check(has_x && all.evictions == 1, "다른 샤드의 x는 그대로, 교체는 1번뿐");
    printf("      (샤드 1개였다면 용량 400000바이트에 넷이 모두 들어가 교체가 없다: 샤드별 LRU는 근사 LRU)\n");
    cache_destroy(c);
}

/* 같은 요청열을 샤드 수만 바꿔 실행하고 HIT/MISS 순서를 기록한다 */
static unsigned long replay(int shards, size_t capacity, unsigned char *hits, int nops,
                            unsigned long *evictions)
{
    struct cache *c = cache_create(capacity, 64 * 1024, shards);
    uint64_t seed = 2026;
    unsigned long nhit = 0;
    char url[96];
    for (int i = 0; i < nops; i++) {
        int key = (int)(rnd(&seed) % 300);
        size_t len = 200 + rnd(&seed) % 8000;
        url_of(url, sizeof url, "seq", key);
        int exp;
        struct cache_obj *o = cache_lookup(c, url, i, 1LL << 40, &exp);
        if (o != NULL) {
            hits[i] = 1;
            nhit++;
            cache_release(c, o);
        } else {
            hits[i] = 0;
            cache_insert(c, url, make_data(url, len), len, "", i);
        }
    }
    struct cache_usage u;
    cache_get_usage(c, &u);
    *evictions = u.evictions;
    if (!shards_within_capacity(c))
        *evictions = (unsigned long)-1;     /* 용량 규칙 위반 표시 */
    cache_destroy(c);
    return nhit;
}

static void test_equivalence(void)
{
    enum { NOPS = 30000 };
    static unsigned char h1[NOPS], h5[NOPS], h8[NOPS];
    unsigned long e1, e5, e8;

    printf("[4] 같은 요청열 %d개 (키 300개): 캐시가 넘치지 않으면 샤드 수와 상관없이 결과가 같아야 함\n", NOPS);
    unsigned long n1 = replay(1, 64 << 20, h1, NOPS, &e1);
    unsigned long n5 = replay(5, 64 << 20, h5, NOPS, &e5);
    unsigned long n8 = replay(8, 64 << 20, h8, NOPS, &e8);
    check(e1 == 0 && e5 == 0 && e8 == 0, "용량 64MB: 교체 0회 (샤드 1·5·8개)");
    check(memcmp(h1, h5, NOPS) == 0 && memcmp(h1, h8, NOPS) == 0,
          "요청 %d개의 HIT/MISS가 하나하나 모두 같음 (HIT %lu·%lu·%lu)", NOPS, n1, n5, n8);

    printf("[5] 같은 요청열, 용량 256KB (교체가 일어나는 경우): 결과가 달라질 수 있음\n");
    n1 = replay(1, 256 << 10, h1, NOPS, &e1);
    n8 = replay(8, 256 << 10, h8, NOPS, &e8);
    printf("      히트율: 샤드 1개 %.1f%% (교체 %lu회) / 샤드 8개 %.1f%% (교체 %lu회)\n",
           100.0 * n1 / NOPS, e1, 100.0 * n8 / NOPS, e8);
    check(e1 != (unsigned long)-1 && e8 != (unsigned long)-1,
          "두 경우 모두 샤드마다 용량 규칙을 지킴 (용량 이하이거나 객체 1개)");
}

/* ---- 여러 스레드가 동시에 ---- */

#define ST_KEYS 256

struct stress_arg {
    struct cache *c;
    char        (*keys)[96];
    int           ops;
    uint64_t      seed;
    unsigned long counted;      /* 이 스레드가 락을 잡게 한 캐시 연산 수 */
    unsigned long hits, bad;
};

static void *stress_worker(void *p)
{
    struct stress_arg *a = p;
    for (int i = 0; i < a->ops; i++) {
        const char *url = a->keys[rnd(&a->seed) % ST_KEYS];
        int exp;
        struct cache_obj *o = cache_lookup(a->c, url, i, 1LL << 40, &exp);
        a->counted++;
        if (o != NULL) {
            a->hits++;
            if (!data_ok(o, url))
                a->bad++;
            if (rnd(&a->seed) % 16 == 0) {
                cache_refresh(a->c, o, i);
                a->counted++;
            }
            cache_release(a->c, o);
            a->counted++;
        } else {
            size_t len = 200 + rnd(&a->seed) % 6000;    /* 객체 상한(샤드 용량)보다 항상 작다 */
            if (cache_insert(a->c, url, make_data(url, len), len, "", i))
                a->counted++;
        }
    }
    return NULL;
}

static void test_stress(int shards, int nthreads, int ops)
{
    static char keys[ST_KEYS][96];
    for (int i = 0; i < ST_KEYS; i++)
        url_of(keys[i], sizeof keys[i], "stress", i);

    /* 용량을 작게 잡아 교체가 계속 일어나게 한다: 다른 스레드가 읽는 중인 객체가
     * 교체되는 경우(참조 카운트로 해제를 미루는 경로)까지 계속 지나가게 */
    struct cache *c = cache_create(96 * 1024, 64 * 1024, shards);
    struct stress_arg args[16];
    pthread_t tid[16];
    for (int t = 0; t < nthreads; t++) {
        args[t] = (struct stress_arg){ c, keys, ops, 0x9E3779B97F4A7C15ULL * (uint64_t)(t + 1), 0, 0, 0 };
        pthread_create(&tid[t], NULL, stress_worker, &args[t]);
    }
    unsigned long counted = 0, hits = 0, bad = 0;
    for (int t = 0; t < nthreads; t++) {
        pthread_join(tid[t], NULL);
        counted += args[t].counted;
        hits += args[t].hits;
        bad += args[t].bad;
    }
    struct cache_usage u;
    cache_get_usage(c, &u);
    unsigned long total = (unsigned long)nthreads * (unsigned long)ops;
    printf("      샤드 %2d개: 연산 %lu개, HIT %.1f%%, 교체 %lu회, 락 획득 %lu회, 경합 %lu회 (%.2f%%)\n",
           shards, total, 100.0 * hits / total, u.evictions, u.lock.acquires, u.lock.contended,
           u.lock.acquires ? 100.0 * u.lock.contended / u.lock.acquires : 0.0);
    check(bad == 0, "샤드 %d개: 읽은 객체의 내용이 모두 자기 URL의 것 (틀린 것 %lu개)", shards, bad);
    check(u.lock.acquires == counted,
          "샤드 %d개: 락 획득 수 = 실제로 부른 캐시 연산 수 (%lu = %lu, 경합 카운터가 빠짐없이 셈)",
          shards, u.lock.acquires, counted);
    check(u.evictions > 0 && shards_within_capacity(c),
          "샤드 %d개: 교체가 일어나는 중에도 샤드마다 용량 규칙을 지킴", shards);
    cache_destroy(c);       /* 남은 객체 해제: ASan이 누수·이중 해제를 검사한다 */
}

/* ================================================================== */
/* 벤치마크                                                             */
/* ================================================================== */

struct bench_arg {
    struct cache   *c;
    char          (*keys)[96];
    const double   *cdf;        /* Zipf 누적 확률 (키 순위별) */
    int             nkeys;
    uint64_t        seed;
    atomic_int     *stop;
    pthread_barrier_t *start;
    unsigned long   ops;
    unsigned long   misses;
};

static int pick_zipf(const double *cdf, int n, uint64_t *seed)
{
    double r = (double)(rnd(seed) >> 11) / 9007199254740992.0;     /* [0, 1) */
    int lo = 0, hi = n - 1;
    while (lo < hi) {                       /* r 이상인 첫 누적 확률 (이진 탐색) */
        int mid = (lo + hi) / 2;
        if (cdf[mid] >= r) hi = mid; else lo = mid + 1;
    }
    return lo;
}

static void *bench_worker(void *p)
{
    struct bench_arg *a = p;
    pthread_barrier_wait(a->start);
    unsigned long n = 0;
    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        for (int i = 0; i < 64; i++) {      /* 정지 신호는 64번에 한 번만 확인 */
            const char *url = a->keys[pick_zipf(a->cdf, a->nkeys, &a->seed)];
            int exp;
            struct cache_obj *o = cache_lookup(a->c, url, 0, 1LL << 40, &exp);
            if (o != NULL)
                cache_release(a->c, o);
            else
                a->misses++;
        }
        n += 64;
    }
    a->ops = n;
    return NULL;
}

static int bench(int argc, char *argv[])
{
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    double secs = 2.0, zipf = 0.8;
    int nkeys = 200;
    char shard_list[128] = "1,2,4,8,16";

    for (int i = 0; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (v == NULL) { fprintf(stderr, "%s 뒤에 값이 필요합니다\n", argv[i]); return 2; }
        if (strcmp(argv[i], "-t") == 0)      threads = atoi(v);
        else if (strcmp(argv[i], "-d") == 0) secs = atof(v);
        else if (strcmp(argv[i], "-k") == 0) nkeys = atoi(v);
        else if (strcmp(argv[i], "-z") == 0) zipf = atof(v);
        else if (strcmp(argv[i], "-S") == 0) snprintf(shard_list, sizeof shard_list, "%s", v);
        else { fprintf(stderr, "알 수 없는 옵션: %s\n", argv[i]); return 2; }
        i++;
    }
    if (threads < 1 || threads > 256 || secs <= 0 || nkeys < 1 || nkeys > 100000 || zipf < 0) {
        fprintf(stderr, "옵션 범위: -t 1~256, -d > 0, -k 1~100000, -z >= 0\n");
        return 2;
    }

    char (*keys)[96] = calloc((size_t)nkeys, sizeof *keys);
    double *cdf = calloc((size_t)nkeys, sizeof *cdf);
    struct bench_arg *args = calloc((size_t)threads, sizeof *args);
    pthread_t *tid = calloc((size_t)threads, sizeof *tid);
    if (!keys || !cdf || !args || !tid) { perror("calloc"); return 2; }

    /* 샤드 수 목록을 먼저 모두 검사한다 */
    int shard_vals[32], nvals = 0;
    char list[128];
    snprintf(list, sizeof list, "%s", shard_list);
    for (char *tok = strtok(list, ","); tok != NULL; tok = strtok(NULL, ",")) {
        int v = atoi(tok);
        if (v < 1 || v > CACHE_MAX_SHARDS || nvals == 32) {
            fprintf(stderr, "샤드 수 '%s': 1~%d만 됩니다 (최대 32개 나열)\n", tok, CACHE_MAX_SHARDS);
            return 2;
        }
        shard_vals[nvals++] = v;
    }
    if (nvals == 0) {
        fprintf(stderr, "-S 뒤에 샤드 수를 쉼표로 나열하세요 (예: 1,2,4,8)\n");
        return 2;
    }

    double total = 0;
    for (int r = 0; r < nkeys; r++) {
        url_of(keys[r], sizeof keys[r], "obj", r);
        total += pow(r + 1, -zipf);         /* 순위 r+1의 가중치 1/(r+1)^s */
        cdf[r] = total;
    }
    for (int r = 0; r < nkeys; r++)
        cdf[r] /= total;
    cdf[nkeys - 1] = 1.0;

    printf("캐시 락 전략 마이크로 벤치마크 (네트워크 없음)\n");
    printf("  스레드 %d개 (CPU %ld개), 샤드마다 %.1f초, 키 %d개, Zipf %.2f → 1위 키가 요청의 %.1f%%\n",
           threads, sysconf(_SC_NPROCESSORS_ONLN), secs, nkeys, zipf, 100.0 * cdf[0]);
    printf("  연산 1개 = cache_lookup + cache_release (프록시 HIT 1건과 같은 락 사용: 캐시 락 2번)\n\n");
    /* 한글은 화면에서 2칸이라 printf의 폭 지정으로는 줄이 맞지 않는다 → 머리줄은 직접 맞춘다 */
    printf("    샤드  처리량(연산/s)      배율      경합률    경합당 대기    1위 키 샤드 몫\n");

    double base = 0;
    for (int vi = 0; vi < nvals; vi++) {
        int shards = shard_vals[vi];
        struct cache *c = cache_create(64 << 20, 64 * 1024, shards);
        if (c == NULL) {
            perror("cache_create");
            return 2;
        }
        for (int r = 0; r < nkeys; r++)     /* 모든 키를 미리 넣어 둔다 → 전부 HIT */
            cache_insert(c, keys[r], make_data(keys[r], 1024), 1024, "", 0);

        /* 1위 키가 든 샤드가 받는 요청 비율 (Zipf에서 샤딩의 한계를 보여 준다) */
        double hot = 0;
        int hot_shard = cache_shard_of(c, keys[0]);
        for (int r = 0; r < nkeys; r++)
            if (cache_shard_of(c, keys[r]) == hot_shard)
                hot += (r == 0 ? cdf[0] : cdf[r] - cdf[r - 1]);

        struct cache_usage before;
        cache_get_usage(c, &before);

        atomic_int stop = 0;
        pthread_barrier_t start;
        pthread_barrier_init(&start, NULL, (unsigned)threads + 1);
        for (int t = 0; t < threads; t++) {
            args[t] = (struct bench_arg){ c, keys, cdf, nkeys, 0x2545F4914F6CDD1DULL * (uint64_t)(t + 7),
                                          &stop, &start, 0, 0 };
            pthread_create(&tid[t], NULL, bench_worker, &args[t]);
        }
        pthread_barrier_wait(&start);
        long long t0 = now_ns();
        struct timespec ts = { (time_t)secs, (long)((secs - (double)(time_t)secs) * 1e9) };
        nanosleep(&ts, NULL);
        atomic_store(&stop, 1);
        unsigned long ops = 0, misses = 0;
        for (int t = 0; t < threads; t++) {
            pthread_join(tid[t], NULL);
            ops += args[t].ops;
            misses += args[t].misses;
        }
        double el = (double)(now_ns() - t0) / 1e9;
        pthread_barrier_destroy(&start);

        struct cache_usage u;
        cache_get_usage(c, &u);
        unsigned long acq = u.lock.acquires - before.lock.acquires;
        unsigned long con = u.lock.contended - before.lock.contended;
        unsigned long long wait = u.lock.wait_ns - before.lock.wait_ns;
        double rate = (double)ops / el;
        if (base == 0)
            base = rate;
        printf("  %6d  %14.0f  %7.2fx  %9.2f%%  %11.2fus  %15.1f%%\n",
               shards, rate, rate / base, acq ? 100.0 * (double)con / (double)acq : 0.0,
               con ? (double)wait / (double)con / 1000.0 : 0.0, 100.0 * hot);
        if (misses != 0)
            printf("    (주의: MISS %lu개 — 모든 키가 들어 있어야 한다)\n", misses);
        cache_destroy(c);
    }
    printf("\n  배율 = 첫 줄(보통 샤드 1개 = 전역 mutex) 대비 처리량\n");
    printf("  경합률 = 락을 잡으려 할 때 다른 스레드가 이미 잡고 있던 비율\n");
    printf("  1위 키 샤드 몫 = 가장 인기 있는 키가 든 샤드로 가는 요청 비율 (샤드를 늘려도 이 아래로는 안 줄어든다)\n");
    free(keys);
    free(cdf);
    free(args);
    free(tid);
    return 0;
}

/* ================================================================== */

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && strcmp(argv[1], "bench") == 0)
        return bench(argc - 2, argv + 2);
    if (argc >= 2) {
        fprintf(stderr, "사용법: %s            모든 검사\n"
                        "        %s bench [-t 스레드] [-d 초] [-k 키 수] [-z Zipf 지수] [-S 1,2,4,8]\n",
                argv[0], argv[0]);
        return 2;
    }

    printf("캐시 점검 (샤딩)\n");
    test_shard_bits();
    test_create();
    test_shard_lru();
    test_equivalence();
    printf("[6] 스레드 8개가 동시에 조회·반납·저장·검증 (스레드마다 연산 30000개, 용량 96KB)\n");
    test_stress(1, 8, 30000);
    test_stress(3, 8, 30000);
    test_stress(8, 8, 30000);

    printf("\n결과: %s\n", g_failures == 0 ? "모두 통과" : "실패 있음");
    return g_failures == 0 ? 0 : 1;
}

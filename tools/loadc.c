/*
 * loadc.c — C로 만든 고부하 HTTP 부하 도구 (6단계: 락 전략별 처리량 측정)
 *
 * Python 부하 생성기(loadgen.py)는 응답 내용까지 확인하는 정확성 도구지만, 초당 수천 개가
 * 한계라서 프록시보다 먼저 포화된다. 이 도구는 스레드 N개가 각자
 *     연결 → 요청 → 응답을 끝까지 읽기 → 닫기
 * 를 쉬지 않고 반복한다 (닫힌 루프: 동시에 진행 중인 요청 수 = 스레드 수로 고정).
 * 그래서 프록시가 감당할 수 있는 최대 처리량을 잴 수 있다.
 *
 * 빌드: make MODE=release loadc        (결과물: build/release/loadc)
 *
 * 사용 예
 *   ./build/release/loadc -x 127.0.0.1:8080 -c 32 -d 10 --prefill -w 2
 *   ./build/release/loadc -x 127.0.0.1:8081,127.0.0.1:8082,127.0.0.1:8083 -n 100000 -c 16
 *   taskset -c 2,3,6,7 ./build/release/loadc -x 127.0.0.1:8080 -c 32 -d 10 --pid 1234 --json r.json
 *
 * 옵션
 *   -x LIST     프록시 주소 목록. 요청 i는 목록의 (i mod 개수)번째로 보낸다 (기본 127.0.0.1:8080)
 *   -o H:P      원서버 주소: 요청 URL은 http://H:P/obj/NNNN.bin (기본 127.0.0.1:9000)
 *   -k N        객체 수: /obj/0000.bin ~ /obj/(N-1).bin (기본 200)
 *   -z S        Zipf 지수 (0이면 균등, 기본 0.8). 순위 r(0부터)의 객체 = /obj/r.bin
 *   -c N        동시 연결 수 = 스레드 수 (기본 8)
 *   -n N        요청 N개를 보내고 끝낸다
 *   -d SEC      SEC초 동안 보낸다 (-n이 없으면 기본 5초)
 *   -w SEC      예열: 측정 전에 SEC초 동안 같은 부하를 걸고 결과는 버린다 (-d와 함께)
 *   -t MS       송수신·연결 제한 시간 (기본 5000)
 *   --prefill   측정 전에 모든 객체를 한 번씩 요청해 캐시를 채운다 (HIT만 재고 싶을 때)
 *   --seed N    난수 시드 (기본 2026)
 *   --pid LIST  측정 동안 이 프로세스들(프록시)이 쓴 CPU 시간도 잰다 (쉼표로 여러 개).
 *               첫 번째 프로세스는 메인 스레드(연결을 받고 작업 스레드를 만드는 accept 스레드)의
 *               CPU 시간도 따로 잰다 → 요청을 하나씩 순서대로 받는 이 스레드가 병목인지 판단
 *   --json FILE 결과를 JSON으로 저장
 *   -q          진행 표시 끔
 *
 * 종료 코드: 0 모든 요청 성공 / 1 실패한 요청 있음 / 2 준비 실패(옵션·주소·예열)
 *
 * 무엇을 "성공"으로 보나: 상태 코드 200, 응답을 연결이 닫힐 때까지 받음,
 *                         Content-Length가 있으면 받은 본문 길이와 같음.
 * 응답 내용(CRC)은 확인하지 않는다 → 내용 검증은 loadgen.py의 몫.
 */
#define _GNU_SOURCE             /* memmem */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROXIES 16
#define MAX_PIDS    8
#define MAX_THREADS 512
#define HEAD_CAP    8192            /* 응답 머리(상태 줄 + 헤더)를 담는 버퍼 */
#define BODY_BUF    65536           /* 나머지 본문은 여기로 읽어 버린다 */

/* ------------------------------------------------------------------ */
/* 결과 분류                                                            */
/* ------------------------------------------------------------------ */

enum err_kind { E_CONNECT, E_SEND, E_TIMEOUT, E_RECV, E_BADRESP, E_SHORT, E_STATUS, E_COUNT };
static const char *const ERR_KEY[E_COUNT]  = { "connect", "send", "timeout", "recv",
                                               "bad_response", "short_body", "status" };
static const char *const ERR_NAME[E_COUNT] = { "연결 실패", "요청 전송 실패", "응답 시간 초과",
                                               "수신 오류", "응답 형식 오류", "본문 잘림",
                                               "200이 아닌 상태 코드" };

/* 응답의 마지막 X-Cache 헤더 = 실제로 처리한 노드의 결과 */
enum label { L_HIT, L_VALIDATED, L_STALE, L_MISS, L_REFRESH, L_FORWARD, L_NONE, L_COUNT };
static const char *const LABEL[L_COUNT] = { "HIT", "VALIDATED", "STALE", "MISS", "REFRESH",
                                            "FORWARD", "none" };

/* ------------------------------------------------------------------ */
/* 설정                                                                 */
/* ------------------------------------------------------------------ */

struct target {
    char               text[64];    /* "127.0.0.1:8080" */
    struct sockaddr_in addr;
};

static struct {
    struct target proxies[MAX_PROXIES];
    int           nproxies;
    char          origin[64];
    int           nkeys;
    double        zipf;
    int           conc;
    long long     nreq;             /* 0이면 시간 모드 */
    double        dur, warm;
    int           timeout_ms;
    uint64_t      seed;
    int           pids[MAX_PIDS];
    int           npids;
    const char   *json;
    int           quiet;
    int           prefill;
} g;

static double            *g_cdf;            /* Zipf 누적 확률 (순위별) */
static atomic_llong       g_next;           /* 다음 요청 번호 (프록시 고르기, -n 세기) */
static atomic_int         g_stop;           /* 1이면 새 요청을 시작하지 않는다 */
static atomic_llong       g_meas_start;     /* 측정 시작 시각(ns). 이후에 시작한 요청만 센다 */

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static uint64_t rnd(uint64_t *s)            /* xorshift64* */
{
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 2685821657736338717ULL;
}

static int pick_key(uint64_t *seed)
{
    double r = (double)(rnd(seed) >> 11) / 9007199254740992.0;     /* [0, 1) */
    int lo = 0, hi = g.nkeys - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_cdf[mid] >= r) hi = mid; else lo = mid + 1;
    }
    return lo;
}

/* ------------------------------------------------------------------ */
/* 스레드                                                               */
/* ------------------------------------------------------------------ */

struct worker {
    pthread_t          tid;
    uint64_t           seed;
    unsigned long      req, ok, err[E_COUNT], label[L_COUNT];
    unsigned long      status[600];     /* 200이 아닌 상태 코드별 개수 */
    unsigned long long bytes;
    uint32_t          *lat;             /* 응답 시간 (마이크로초) */
    size_t             nlat, caplat;
    unsigned char     *seen;            /* 요청한 객체 표시 (서로 다른 객체 수) */
    char               head[HEAD_CAP];
    char               body[BODY_BUF];
};

/* 헤더 줄들(start ~ end)에서 이름이 name인 마지막 줄의 값 시작 위치. 없으면 NULL */
static const char *find_header(const char *start, const char *end, const char *name, int last)
{
    size_t nlen = strlen(name);
    const char *found = NULL;
    const char *p = start;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (eol == NULL)
            eol = end;
        if ((size_t)(eol - p) > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            found = v;
            if (!last)
                return found;
        }
        p = eol + 1;
    }
    return found;
}

static enum label parse_label(const char *v)
{
    if (v == NULL)
        return L_NONE;
    for (int i = 0; i < L_NONE; i++) {
        size_t n = strlen(LABEL[i]);
        if (strncmp(v, LABEL[i], n) == 0 && (v[n] == ' ' || v[n] == '\r' || v[n] == '\n'))
            return (enum label)i;
    }
    return L_NONE;
}

/* 요청 하나. 성공이면 -1, 실패면 enum err_kind. *label, *status, *bytes를 채운다 */
static int do_request(struct worker *w, const struct target *t, int key,
                      enum label *label, int *status, long long *bytes)
{
    *label = L_NONE;
    *status = 0;
    *bytes = 0;

    char req[256];
    int rl = snprintf(req, sizeof req,
                      "GET http://%s/obj/%04d.bin HTTP/1.0\r\nHost: %s\r\nUser-Agent: loadc\r\n\r\n",
                      g.origin, key, g.origin);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return E_CONNECT;
    /* 리눅스에서 SO_SNDTIMEO는 connect()의 제한 시간에도 쓰인다 */
    struct timeval tv = { g.timeout_ms / 1000, (g.timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, (const struct sockaddr *)&t->addr, sizeof t->addr) < 0) {
        close(fd);
        return E_CONNECT;
    }
    for (int off = 0; off < rl;) {
        ssize_t n = send(fd, req + off, (size_t)(rl - off), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return E_SEND;
        }
        off += (int)n;
    }

    /* 연결이 닫힐 때(EOF)까지 읽는다. 처음 HEAD_CAP바이트는 헤더를 보려고 남겨 둔다 */
    size_t have = 0;
    long long total = 0;
    for (;;) {
        char  *dst = have < HEAD_CAP ? w->head + have : w->body;
        size_t cap = have < HEAD_CAP ? HEAD_CAP - have : BODY_BUF;
        ssize_t n = recv(fd, dst, cap, 0);
        if (n > 0) {
            if (have < HEAD_CAP)
                have += (size_t)n;
            total += n;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        int e = (errno == EAGAIN || errno == EWOULDBLOCK) ? E_TIMEOUT : E_RECV;
        close(fd);
        *bytes = total;
        return e;
    }
    close(fd);
    *bytes = total;

    /* 상태 줄: "HTTP/1.x NNN ..." */
    if (have < 12 || memcmp(w->head, "HTTP/1.", 7) != 0)
        return E_BADRESP;
    *status = atoi(w->head + 9);
    const char *hend = memmem(w->head, have, "\r\n\r\n", 4);
    if (hend == NULL)
        return E_BADRESP;
    const char *lines = memchr(w->head, '\n', have) + 1;
    long long hlen = (long long)(hend - w->head) + 4;

    *label = parse_label(find_header(lines, hend + 2, "X-Cache", 1));
    const char *cl = find_header(lines, hend + 2, "Content-Length", 0);
    if (cl != NULL && strtoll(cl, NULL, 10) != total - hlen)
        return E_SHORT;
    if (*status != 200)
        return E_STATUS;
    return -1;
}

static void record_latency(struct worker *w, long long ns)
{
    if (w->nlat == w->caplat) {
        size_t ncap = w->caplat ? w->caplat * 2 : 65536;
        uint32_t *p = realloc(w->lat, ncap * sizeof *p);
        if (p == NULL)
            return;                     /* 메모리 부족: 표본 하나를 버린다 */
        w->lat = p;
        w->caplat = ncap;
    }
    long long us = ns / 1000;
    w->lat[w->nlat++] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    while (!atomic_load(&g_stop)) {
        long long i = atomic_fetch_add(&g_next, 1);
        if (g.nreq > 0 && i >= g.nreq)
            break;
        const struct target *t = &g.proxies[i % g.nproxies];
        int key = pick_key(&w->seed);

        long long t0 = now_ns();
        enum label label;
        int status;
        long long bytes;
        int r = do_request(w, t, key, &label, &status, &bytes);
        long long t1 = now_ns();

        /* 측정 구간 안에서 시작해서, 정지 신호 전에 끝난 요청만 센다 */
        if (t0 < atomic_load(&g_meas_start) || atomic_load(&g_stop))
            continue;
        w->req++;
        w->bytes += (unsigned long long)bytes;
        w->label[label]++;
        w->seen[key] = 1;
        if (r < 0) {
            w->ok++;
            record_latency(w, t1 - t0);
        } else {
            w->err[r]++;
            if (r == E_STATUS && status >= 100 && status < 600)
                w->status[status]++;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CPU 사용 시간                                                        */
/* ------------------------------------------------------------------ */

static double self_cpu_sec(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
         + (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* /proc/PID/stat의 utime + stime (초). 프로세스가 없으면 -1.
 * tid > 0이면 그 스레드 하나만: /proc/PID/task/TID/stat (메인 스레드의 TID = PID) */
static double proc_cpu_sec_of(int pid, int tid)
{
    char path[64], buf[1024];
    if (tid > 0)
        snprintf(path, sizeof path, "/proc/%d/task/%d/stat", pid, tid);
    else
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    /* 두 번째 칸(실행 파일 이름)은 괄호 안에 공백이 있을 수 있으므로 마지막 ')' 뒤부터 센다.
     * ')' 뒤 첫 칸이 3번째 필드(state), utime은 14번째, stime은 15번째 */
    char *p = strrchr(buf, ')');
    if (p == NULL)
        return -1;
    unsigned long long ut = 0, st = 0;
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2)
        return -1;
    return (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
}

static double proc_cpu_sec(int pid) { return proc_cpu_sec_of(pid, 0); }

/* 첫 번째 --pid 프로세스의 메인 스레드 CPU 시간 (없으면 -1) */
static double main_thread_cpu_sec(void)
{
    return g.npids > 0 ? proc_cpu_sec_of(g.pids[0], g.pids[0]) : -1;
}

static double pids_cpu_sec(void)
{
    double sum = 0;
    for (int i = 0; i < g.npids; i++) {
        double s = proc_cpu_sec(g.pids[i]);
        if (s < 0)
            return -1;
        sum += s;
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* 옵션                                                                 */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
            "사용법: loadc [-x 프록시목록] [-o 원서버] [-k 객체수] [-z Zipf] [-c 동시] [-n 요청수 | -d 초]\n"
            "             [-w 예열초] [-t 제한ms] [--prefill] [--seed N] [--pid 목록] [--json 파일] [-q]\n"
            "자세한 설명은 tools/loadc.c 맨 위 주석을 보세요.\n");
}

static int parse_target(const char *s, struct target *t)
{
    char host[64];
    const char *colon = strrchr(s, ':');
    if (colon == NULL || colon == s || (size_t)(colon - s) >= sizeof host)
        return -1;
    memcpy(host, s, (size_t)(colon - s));
    host[colon - s] = '\0';
    char *end;
    long port = strtol(colon + 1, &end, 10);
    if (*end != '\0' || port < 1 || port > 65535)
        return -1;

    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL)
        return -1;
    memcpy(&t->addr, res->ai_addr, sizeof t->addr);
    freeaddrinfo(res);
    t->addr.sin_port = htons((uint16_t)port);
    snprintf(t->text, sizeof t->text, "%s", s);
    return 0;
}

static int parse_args(int argc, char *argv[])
{
    g.nkeys = 200;
    g.zipf = 0.8;
    g.conc = 8;
    g.timeout_ms = 5000;
    g.seed = 2026;
    snprintf(g.origin, sizeof g.origin, "127.0.0.1:9000");
    const char *proxies = "127.0.0.1:8080";
    const char *pids = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-q") == 0)         { g.quiet = 1; continue; }
        if (strcmp(a, "--prefill") == 0)  { g.prefill = 1; continue; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) return -1;
        if (i + 1 >= argc) {
            fprintf(stderr, "%s 뒤에 값이 필요합니다\n", a);
            return -1;
        }
        const char *v = argv[++i];
        if (strcmp(a, "-x") == 0)            proxies = v;
        else if (strcmp(a, "-o") == 0)       snprintf(g.origin, sizeof g.origin, "%s", v);
        else if (strcmp(a, "-k") == 0)       g.nkeys = atoi(v);
        else if (strcmp(a, "-z") == 0)       g.zipf = atof(v);
        else if (strcmp(a, "-c") == 0)       g.conc = atoi(v);
        else if (strcmp(a, "-n") == 0)       g.nreq = atoll(v);
        else if (strcmp(a, "-d") == 0)       g.dur = atof(v);
        else if (strcmp(a, "-w") == 0)       g.warm = atof(v);
        else if (strcmp(a, "-t") == 0)       g.timeout_ms = atoi(v);
        else if (strcmp(a, "--seed") == 0)   g.seed = strtoull(v, NULL, 10);
        else if (strcmp(a, "--pid") == 0)    pids = v;
        else if (strcmp(a, "--json") == 0)   g.json = v;
        else {
            fprintf(stderr, "알 수 없는 옵션: %s\n", a);
            return -1;
        }
    }

    if (g.nkeys < 1 || g.nkeys > 10000 || g.zipf < 0 || g.conc < 1 || g.conc > MAX_THREADS
        || g.nreq < 0 || g.dur < 0 || g.warm < 0 || g.timeout_ms < 1) {
        fprintf(stderr, "옵션 범위: -k 1~10000, -z >= 0, -c 1~%d, -t >= 1\n", MAX_THREADS);
        return -1;
    }
    if (g.nreq > 0 && g.dur > 0) {
        fprintf(stderr, "-n과 -d는 함께 쓸 수 없습니다\n");
        return -1;
    }
    if (g.nreq > 0 && g.warm > 0) {
        fprintf(stderr, "-w(예열)는 -d와 함께 씁니다\n");
        return -1;
    }
    if (g.nreq == 0 && g.dur == 0)
        g.dur = 5;

    char list[512];
    snprintf(list, sizeof list, "%s", proxies);
    for (char *tok = strtok(list, ","); tok != NULL; tok = strtok(NULL, ",")) {
        if (g.nproxies == MAX_PROXIES || parse_target(tok, &g.proxies[g.nproxies]) < 0) {
            fprintf(stderr, "프록시 주소가 잘못됐거나 너무 많습니다: %s\n", tok);
            return -1;
        }
        g.nproxies++;
    }
    if (pids != NULL) {
        snprintf(list, sizeof list, "%s", pids);
        for (char *tok = strtok(list, ","); tok != NULL; tok = strtok(NULL, ",")) {
            if (g.npids == MAX_PIDS || atoi(tok) <= 0) {
                fprintf(stderr, "--pid 값이 잘못됐습니다: %s\n", tok);
                return -1;
            }
            g.pids[g.npids++] = atoi(tok);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 결과                                                                 */
/* ------------------------------------------------------------------ */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* 최근접 순위 방식: 정렬한 값에서 ceil(q × n)번째 (loadgen.py와 같은 정의) */
static double pct_ms(const uint32_t *v, size_t n, double q)
{
    if (n == 0)
        return 0;
    size_t k = (size_t)ceil(q * (double)n);
    if (k < 1)
        k = 1;
    return v[k - 1] / 1000.0;
}

/* 스레드 자료와 누적 확률 해제 (정상 종료와 준비 실패 모두에서 부른다) */
static void free_workers(struct worker *ws)
{
    if (ws != NULL)
        for (int i = 0; i < g.conc; i++) {
            free(ws[i].lat);
            free(ws[i].seen);
        }
    free(ws);
    free(g_cdf);
    g_cdf = NULL;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (parse_args(argc, argv) < 0) {
        usage();
        return 2;
    }

    g_cdf = calloc((size_t)g.nkeys, sizeof *g_cdf);
    struct worker *ws = calloc((size_t)g.conc, sizeof *ws);
    if (g_cdf == NULL || ws == NULL) {
        perror("calloc");
        free_workers(ws);
        return 2;
    }
    double total = 0;
    for (int r = 0; r < g.nkeys; r++) {
        total += pow(r + 1, -g.zipf);       /* 순위 r+1의 가중치 1/(r+1)^s */
        g_cdf[r] = total;
    }
    for (int r = 0; r < g.nkeys; r++)
        g_cdf[r] /= total;
    g_cdf[g.nkeys - 1] = 1.0;

    for (int i = 0; i < g.conc; i++) {
        ws[i].seed = (g.seed + 1) * 0x9E3779B97F4A7C15ULL + (uint64_t)i * 0xBF58476D1CE4E5B9ULL;
        if (ws[i].seed == 0)
            ws[i].seed = 1;
        ws[i].seen = calloc((size_t)g.nkeys, 1);
        if (ws[i].seen == NULL) {
            perror("calloc");
            free_workers(ws);
            return 2;
        }
    }

    /* 캐시 미리 채우기: 객체마다 한 번씩 (한 스레드, 프록시 목록을 돌아가며) */
    if (g.prefill) {
        int bad = 0;
        for (int k = 0; k < g.nkeys; k++) {
            enum label l;
            int st;
            long long b;
            if (do_request(&ws[0], &g.proxies[k % g.nproxies], k, &l, &st, &b) >= 0)
                bad++;
        }
        if (bad > 0) {
            fprintf(stderr, "캐시 채우기에서 %d개가 실패했습니다. 프록시와 원서버가 켜져 있는지 확인하세요.\n", bad);
            free_workers(ws);
            return 2;
        }
    }

    /* 시간 모드에서 예열이 있으면, 측정 시작 시각을 나중에 정한다 (그 전 요청은 세지 않음) */
    atomic_store(&g_meas_start, (g.dur > 0 && g.warm > 0) ? LLONG_MAX : 0);
    long long start = now_ns();
    for (int i = 0; i < g.conc; i++)
        if (pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]) != 0) {
            perror("pthread_create");
            exit(2);            /* 이미 도는 스레드가 있으므로 정리하지 않고 끝낸다 */
        }

    long long meas_start = start, meas_end;
    double cpu0, pcpu0, mcpu0;
    if (g.dur > 0) {
        if (g.warm > 0) {
            struct timespec ts = { (time_t)g.warm, (long)((g.warm - (double)(time_t)g.warm) * 1e9) };
            nanosleep(&ts, NULL);
            meas_start = now_ns();
            atomic_store(&g_meas_start, meas_start);
        }
        cpu0 = self_cpu_sec();
        pcpu0 = pids_cpu_sec();
        mcpu0 = main_thread_cpu_sec();
        long long end = meas_start + (long long)(g.dur * 1e9);
        for (;;) {
            long long left = end - now_ns();
            if (left <= 0)
                break;
            long long step = left < 1000000000LL ? left : 1000000000LL;
            struct timespec ts = { (time_t)(step / 1000000000LL), (long)(step % 1000000000LL) };
            nanosleep(&ts, NULL);
            if (!g.quiet)       /* 진행 표시: 시작한 요청 수 (예열 포함, 원자 변수라 경쟁 없음) */
                fprintf(stderr, "\r  측정 중 %.0f/%.0f초, 보낸 요청 %lld개   ",
                        (double)(now_ns() - meas_start) / 1e9, g.dur, (long long)atomic_load(&g_next));
        }
        meas_end = now_ns();
        atomic_store(&g_stop, 1);
        for (int i = 0; i < g.conc; i++)
            pthread_join(ws[i].tid, NULL);
    } else {
        cpu0 = self_cpu_sec();
        pcpu0 = pids_cpu_sec();
        mcpu0 = main_thread_cpu_sec();
        for (int i = 0; i < g.conc; i++)
            pthread_join(ws[i].tid, NULL);
        meas_end = now_ns();
    }
    double cpu1 = self_cpu_sec(), pcpu1 = pids_cpu_sec(), mcpu1 = main_thread_cpu_sec();
    if (!g.quiet && g.dur > 0)
        fprintf(stderr, "\r%60s\r", "");

    /* ---- 합치기 ---- */
    double el = (double)(meas_end - meas_start) / 1e9;
    unsigned long req = 0, ok = 0, err[E_COUNT] = { 0 }, label[L_COUNT] = { 0 };
    static unsigned long status[600];
    unsigned long long bytes = 0;
    size_t nlat = 0;
    for (int i = 0; i < g.conc; i++) {
        req += ws[i].req;
        ok += ws[i].ok;
        bytes += ws[i].bytes;
        nlat += ws[i].nlat;
        for (int k = 0; k < E_COUNT; k++) err[k] += ws[i].err[k];
        for (int k = 0; k < L_COUNT; k++) label[k] += ws[i].label[k];
        for (int k = 0; k < 600; k++) status[k] += ws[i].status[k];
    }
    uint32_t *lat = malloc((nlat ? nlat : 1) * sizeof *lat);
    if (lat == NULL) {
        perror("malloc");
        free_workers(ws);
        return 2;
    }
    size_t off = 0;
    double sum_ms = 0;
    for (int i = 0; i < g.conc; i++) {
        if (ws[i].nlat == 0)
            continue;       /* 성공한 요청이 없는 스레드: lat가 NULL. memcpy에 NULL은 크기 0이어도 안 된다 */
        memcpy(lat + off, ws[i].lat, ws[i].nlat * sizeof *lat);
        off += ws[i].nlat;
    }
    for (size_t i = 0; i < nlat; i++)
        sum_ms += lat[i] / 1000.0;
    qsort(lat, nlat, sizeof *lat, cmp_u32);
    int distinct = 0;
    for (int k = 0; k < g.nkeys; k++)
        for (int i = 0; i < g.conc; i++)
            if (ws[i].seen[k]) {
                distinct++;
                break;
            }

    unsigned long failed = req - ok;
    double rps = el > 0 ? req / el : 0;
    double self_cores = el > 0 ? (cpu1 - cpu0) / el : 0;
    int pid_ok = g.npids > 0 && pcpu0 >= 0 && pcpu1 >= 0;
    double proxy_cores = pid_ok && el > 0 ? (pcpu1 - pcpu0) / el : -1;
    double proxy_us_per_req = pid_ok && req > 0 ? (pcpu1 - pcpu0) * 1e6 / (double)req : -1;
    int main_ok = pid_ok && mcpu0 >= 0 && mcpu1 >= 0;
    double main_cores = main_ok && el > 0 ? (mcpu1 - mcpu0) / el : -1;
    double main_us_per_req = main_ok && req > 0 ? (mcpu1 - mcpu0) * 1e6 / (double)req : -1;
    double mean = nlat ? sum_ms / (double)nlat : 0;
    double p50 = pct_ms(lat, nlat, 0.50), p90 = pct_ms(lat, nlat, 0.90);
    double p99 = pct_ms(lat, nlat, 0.99), pmax = nlat ? lat[nlat - 1] / 1000.0 : 0;

    /* ---- 화면 ---- */
    printf("loadc 결과\n");
    printf("  대상: 프록시 %d개 (", g.nproxies);
    for (int i = 0; i < g.nproxies; i++)
        printf("%s%s", i ? ", " : "", g.proxies[i].text);
    printf("), 원서버 %s, 객체 %d개, Zipf %.2f\n", g.origin, g.nkeys, g.zipf);
    printf("  동시 연결 %d, 측정 %.2f초%s%s\n", g.conc, el,
           g.warm > 0 ? " (예열 뒤)" : "", g.prefill ? ", 캐시 미리 채움" : "");
    printf("  요청 %lu개: 성공 %lu, 실패 %lu\n", req, ok, failed);
    for (int k = 0; k < E_COUNT; k++)
        if (err[k])
            printf("    %s %lu\n", ERR_NAME[k], err[k]);
    for (int k = 100; k < 600; k++)
        if (status[k])
            printf("    상태 %d: %lu\n", k, status[k]);
    printf("  처리량 %.1f 요청/s, %.1f MB/s\n", rps, el > 0 ? (double)bytes / el / 1048576.0 : 0);
    printf("  응답 시간(ms, 성공한 요청): 평균 %.3f  p50 %.3f  p90 %.3f  p99 %.3f  최대 %.3f\n",
           mean, p50, p90, p99, pmax);
    printf("  결과(마지막 X-Cache):");
    for (int k = 0; k < L_COUNT; k++)
        if (label[k])
            printf(" %s %lu", LABEL[k], label[k]);
    printf(" | 서로 다른 객체 %d개\n", distinct);
    printf("  CPU: 부하 도구 %.2f코어", self_cores);
    if (pid_ok)
        printf(", 프록시 %.2f코어 → 요청 하나에 프록시 CPU %.1fus", proxy_cores, proxy_us_per_req);
    if (main_ok)
        printf("\n       그중 메인(accept) 스레드 %.2f코어 → 요청 하나에 %.1fus", main_cores, main_us_per_req);
    else if (g.npids > 0)
        printf(", 프록시 CPU 측정 실패 (--pid 프로세스를 찾을 수 없음)");
    printf("\n  (코어 = CPU 시간 ÷ 측정 시간. 1.00이면 CPU 하나를 꽉 채워 쓴 것.\n"
           "   메인 스레드는 혼자서 연결을 차례로 받으므로 1.00에 가까우면 그 스레드가 병목이다)\n");

    /* ---- JSON ---- */
    if (g.json != NULL) {
        FILE *fp = fopen(g.json, "w");
        if (fp == NULL) {
            perror(g.json);
            free(lat);
            free_workers(ws);
            return 2;
        }
        fprintf(fp, "{\"config\":{\"proxies\":[");
        for (int i = 0; i < g.nproxies; i++)
            fprintf(fp, "%s\"%s\"", i ? "," : "", g.proxies[i].text);
        fprintf(fp, "],\"origin\":\"%s\",\"keys\":%d,\"zipf\":%.4f,\"concurrency\":%d,"
                    "\"requests\":%lld,\"duration\":%.3f,\"warmup\":%.3f,\"prefill\":%s,\"seed\":%llu},\n",
                g.origin, g.nkeys, g.zipf, g.conc, g.nreq, g.dur, g.warm,
                g.prefill ? "true" : "false", (unsigned long long)g.seed);
        fprintf(fp, " \"result\":{\"requests\":%lu,\"ok\":%lu,\"failed\":%lu,\"elapsed_sec\":%.6f,"
                    "\"rps\":%.3f,\"bytes\":%llu,\"distinct\":%d,\n  \"errors\":{",
                req, ok, failed, el, rps, bytes, distinct);
        for (int k = 0; k < E_COUNT; k++)
            fprintf(fp, "%s\"%s\":%lu", k ? "," : "", ERR_KEY[k], err[k]);
        fprintf(fp, "},\"status\":{");
        int first = 1;
        for (int k = 100; k < 600; k++)
            if (status[k]) {
                fprintf(fp, "%s\"%d\":%lu", first ? "" : ",", k, status[k]);
                first = 0;
            }
        fprintf(fp, "},\n  \"labels\":{");
        for (int k = 0; k < L_COUNT; k++)
            fprintf(fp, "%s\"%s\":%lu", k ? "," : "", LABEL[k], label[k]);
        fprintf(fp, "},\n  \"latency_ms\":{\"mean\":%.4f,\"p50\":%.4f,\"p90\":%.4f,\"p99\":%.4f,\"max\":%.4f},\n",
                mean, p50, p90, p99, pmax);
        fprintf(fp, "  \"cpu\":{\"loader_cores\":%.4f", self_cores);
        if (pid_ok)
            fprintf(fp, ",\"proxy_cores\":%.4f,\"proxy_us_per_req\":%.3f", proxy_cores, proxy_us_per_req);
        else
            fprintf(fp, ",\"proxy_cores\":null,\"proxy_us_per_req\":null");
        if (main_ok)
            fprintf(fp, ",\"proxy_main_cores\":%.4f,\"proxy_main_us_per_req\":%.3f", main_cores, main_us_per_req);
        else
            fprintf(fp, ",\"proxy_main_cores\":null,\"proxy_main_us_per_req\":null");
        fprintf(fp, "}}}\n");
        fclose(fp);
    }

    free(lat);
    free_workers(ws);
    return failed == 0 ? 0 : 1;
}

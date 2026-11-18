/*
 * router.c — CDN 라우터: 랑데부 해싱, 노드 목록, Via 검사, 장애 표시
 *
 * 노드 목록은 프로그램 시작 때 한 번 읽고 이후로는 읽기만 한다 (락 불필요).
 * 장애 표시만 여러 스레드가 함께 쓰므로 원자 변수(_Atomic)를 쓴다.
 */
#include "router.h"
#include "hash.h"
#include "http.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static struct node g_nodes[ROUTER_MAX_NODES];
static uint64_t    g_seeds[ROUTER_MAX_NODES];
static int         g_count;
static int         g_self = -1;
static int         g_enabled;

/* 노드별 "이 시각까지는 건너뜀" (단조 시계 ms). 0이면 정상 */
static _Atomic long long g_down_until[ROUTER_MAX_NODES];

/* ------------------------------------------------------------------ */
/* 랑데부 해싱                                                          */
/* ------------------------------------------------------------------ */

uint64_t rendezvous_key(const char *key)
{
    return hash_fnv1a(key);
}

uint64_t rendezvous_seed(const char *node_name)
{
    return hash_mix64(hash_fnv1a(node_name));
}

/* 점수 = mix(URL 해시 XOR 노드 seed).
 * mix가 눈사태 효과를 가진 일대일 함수라서, 같은 URL이라도 노드마다
 * 서로 독립적인 난수처럼 보이는 점수가 나온다 → 담당이 노드들에 고르게 퍼진다.
 * URL 해시는 요청마다 한 번만 계산하고, 노드 수만큼 mix만 반복한다 */
uint64_t rendezvous_score(uint64_t seed, uint64_t key_hash)
{
    return hash_mix64(key_hash ^ seed);
}

void rendezvous_rank(const uint64_t *seeds, int n, const char *key,
                     int *order, uint64_t *scores)
{
    if (n > ROUTER_MAX_NODES)
        n = ROUTER_MAX_NODES;

    uint64_t kh = rendezvous_key(key);
    uint64_t s[ROUTER_MAX_NODES];

    /* 삽입 정렬 (노드는 몇 개뿐이라 충분히 빠르다).
     * 앞 원소의 점수가 "작을 때만" 밀어내므로, 점수가 같으면 번호가 작은 노드가 앞에 남는다 */
    for (int i = 0; i < n; i++) {
        s[i] = rendezvous_score(seeds[i], kh);
        int j = i;
        while (j > 0 && s[order[j - 1]] < s[i]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    if (scores != NULL)
        for (int i = 0; i < n; i++)
            scores[i] = s[order[i]];
}

/* ------------------------------------------------------------------ */
/* 노드 목록 파일                                                       */
/* ------------------------------------------------------------------ */

/* 이름은 영문·숫자·'-'·'_'·'.'만: Via 헤더 안에 그대로 들어가므로 공백·쉼표 금지 */
static int valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= ROUTER_NAME_MAX)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.')
            return 0;
    return 1;
}

int router_load(const char *path, const char *self_name, char *err, size_t errcap)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        snprintf(err, errcap, "노드 목록 파일을 열 수 없습니다: %s", path);
        return -1;
    }

    struct node nodes[ROUTER_MAX_NODES];
    int n = 0, lineno = 0;
    char line[512];

    while (fgets(line, sizeof line, fp) != NULL) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';

        char name[64], host[128], extra[8];
        int port;
        int k = sscanf(line, "%63s %127s %d %7s", name, host, &port, extra);
        if (k <= 0)
            continue;                                   /* 빈 줄 */
        if (k != 3) {
            snprintf(err, errcap, "%s %d번째 줄: '이름 호스트 포트' 세 칸이어야 합니다", path, lineno);
            goto fail;
        }
        if (!valid_name(name)) {
            snprintf(err, errcap, "%s %d번째 줄: 이름 '%s'에는 영문·숫자·-·_·.만 쓸 수 있습니다 (최대 %d자)",
                     path, lineno, name, ROUTER_NAME_MAX - 1);
            goto fail;
        }
        if (strlen(host) >= ROUTER_HOST_MAX) {
            snprintf(err, errcap, "%s %d번째 줄: 호스트가 너무 깁니다", path, lineno);
            goto fail;
        }
        if (port < 1 || port > 65535) {
            snprintf(err, errcap, "%s %d번째 줄: 포트 %d가 범위(1~65535)를 벗어납니다", path, lineno, port);
            goto fail;
        }
        if (n == ROUTER_MAX_NODES) {
            snprintf(err, errcap, "노드는 최대 %d개까지입니다", ROUTER_MAX_NODES);
            goto fail;
        }
        for (int i = 0; i < n; i++) {
            if (strcasecmp(nodes[i].name, name) == 0) {
                snprintf(err, errcap, "%s %d번째 줄: 이름 '%s'가 중복됩니다", path, lineno, name);
                goto fail;
            }
            if (strcmp(nodes[i].host, host) == 0 && nodes[i].port == port) {
                snprintf(err, errcap, "%s %d번째 줄: 주소 %s:%d가 중복됩니다", path, lineno, host, port);
                goto fail;
            }
        }
        /* 길이는 위에서 검사했다. 정밀도(%.*s)로 최대 길이를 컴파일러에게도 알려 준다 */
        snprintf(nodes[n].name, sizeof nodes[n].name, "%.*s", ROUTER_NAME_MAX - 1, name);
        snprintf(nodes[n].host, sizeof nodes[n].host, "%.*s", ROUTER_HOST_MAX - 1, host);
        nodes[n].port = port;
        nodes[n].seed = rendezvous_seed(name);
        n++;
    }
    fclose(fp);
    fp = NULL;

    if (n == 0) {
        snprintf(err, errcap, "%s에 노드가 하나도 없습니다", path);
        return -1;
    }

    int self = -1;
    if (self_name != NULL) {
        for (int i = 0; i < n; i++)
            if (strcasecmp(nodes[i].name, self_name) == 0)
                self = i;
        if (self < 0) {
            snprintf(err, errcap, "이 노드의 이름 '%s'가 %s에 없습니다 (-n 옵션 확인)", self_name, path);
            return -1;
        }
    }

    /* 모두 검사한 뒤에만 전역에 반영한다 */
    for (int i = 0; i < n; i++) {
        g_nodes[i] = nodes[i];
        g_seeds[i] = nodes[i].seed;
        atomic_store(&g_down_until[i], 0);
    }
    g_count   = n;
    g_self    = self;
    g_enabled = 1;
    return 0;

fail:
    if (fp)
        fclose(fp);
    return -1;
}

int router_enabled(void) { return g_enabled; }
int router_count(void)   { return g_count; }
int router_self(void)    { return g_self; }

const struct node *router_node(int i)
{
    return (i >= 0 && i < g_count) ? &g_nodes[i] : NULL;
}

int router_find(const char *name)
{
    for (int i = 0; i < g_count; i++)
        if (strcasecmp(g_nodes[i].name, name) == 0)
            return i;
    return -1;
}

int router_rank(const char *key, int *order, uint64_t *scores)
{
    rendezvous_rank(g_seeds, g_count, key, order, scores);
    return g_count;
}

/* ------------------------------------------------------------------ */
/* Via 검사 (루프 방지)                                                 */
/* ------------------------------------------------------------------ */

/* Via 값의 형식 (RFC 9110 7.6.3):
 *   Via: 1.1 node1, 1.0 node2 (주석)
 *   원소 = 받은-프로토콜 공백 받은-노드 [공백 주석], 원소끼리는 쉼표로 구분 */
int router_via_peer(const struct http_request *req)
{
    if (!g_enabled)
        return -1;

    int found = -1;
    for (int h = 0; h < req->nheaders; h++) {
        if (strcasecmp(req->headers[h].name, "Via") != 0)
            continue;

        const char *v = req->headers[h].value;
        while (*v != '\0') {
            while (*v == ' ' || *v == '\t' || *v == ',')
                v++;
            if (*v == '\0')
                break;
            const char *end = strchr(v, ',');
            if (end == NULL)
                end = v + strlen(v);

            /* 첫 토큰(받은 프로토콜, 예: "1.1")을 건너뛰고 두 번째 토큰(노드 이름)을 본다 */
            const char *p = v;
            while (p < end && *p != ' ' && *p != '\t')
                p++;
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            const char *by = p;
            while (p < end && *p != ' ' && *p != '\t')
                p++;

            size_t len = (size_t)(p - by);
            if (len > 0 && len < ROUTER_NAME_MAX) {
                char name[ROUTER_NAME_MAX];
                memcpy(name, by, len);
                name[len] = '\0';
                int idx = router_find(name);
                if (idx >= 0)
                    found = idx;        /* 뒤에 나올수록 최근에 거친 노드 */
            }
            v = end;
        }
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* 장애 표시                                                            */
/* ------------------------------------------------------------------ */

int router_available(int i, long long now_ms)
{
    long long until = atomic_load(&g_down_until[i]);
    return until == 0 || now_ms >= until;
}

int router_mark_down(int i, long long now_ms, int retry_ms)
{
    long long prev = atomic_exchange(&g_down_until[i], now_ms + retry_ms);
    return prev == 0;
}

int router_mark_up(int i)
{
    if (atomic_load(&g_down_until[i]) == 0)
        return 0;           /* 대부분의 경우: 이미 정상. 쓰기를 피해 스레드 간 경합을 줄인다 */
    return atomic_exchange(&g_down_until[i], 0) != 0;
}

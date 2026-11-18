/*
 * rendezvous_test.c — 랑데부 해싱 단위 테스트, 분포·재배치 실험, 담당 노드 조회
 *
 * 빌드: make rtest            (결과물: build/<모드>/rendezvous_test)
 *
 *   ./build/debug/rendezvous_test
 *       모든 검사를 실행한다. 하나라도 실패하면 종료 코드 1
 *   ./build/debug/rendezvous_test rank URL...
 *   ./build/debug/rendezvous_test rank -c conf/cluster.conf URL...
 *       노드 목록 파일 기준으로 URL마다 노드 순위(1순위 = 담당)를 보여 준다
 */
#include "hash.h"
#include "router.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NKEYS 120000

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

static void key_of(char *buf, size_t cap, int i)
{
    snprintf(buf, cap, "http://127.0.0.1:9000/obj/%06d.bin", i);
}

static int owner_of(const uint64_t *seeds, int n, const char *key)
{
    int order[ROUTER_MAX_NODES];
    rendezvous_rank(seeds, n, key, order, NULL);
    return order[0];
}

/* 비교 대상: 해시값을 노드 수로 나눈 나머지로 담당을 정하는 방식 (hash % N) */
static int modn_owner(const char *key, int n)
{
    return (int)(hash_mix64(rendezvous_key(key)) % (uint64_t)n);
}

/* ------------------------------------------------------------------ */

static void test_basic(void)
{
    printf("[1] 결정성과 순위의 형태\n");
    const char *names[] = { "node1", "node2", "node3", "node4", "node5" };
    uint64_t seeds[5];
    for (int i = 0; i < 5; i++)
        seeds[i] = rendezvous_seed(names[i]);

    int same = 1, perm = 1, desc = 1;
    char key[96];
    for (int k = 0; k < 2000; k++) {
        key_of(key, sizeof key, k);
        int a[5], b[5];
        uint64_t sc[5];
        rendezvous_rank(seeds, 5, key, a, sc);
        rendezvous_rank(seeds, 5, key, b, NULL);
        if (memcmp(a, b, sizeof a) != 0)
            same = 0;
        int seen[5] = { 0 };
        for (int i = 0; i < 5; i++) {
            if (a[i] < 0 || a[i] >= 5 || seen[a[i]]++)
                perm = 0;
            if (i > 0 && sc[i - 1] < sc[i])
                desc = 0;
        }
    }
    check(same, "같은 URL은 언제 계산해도 같은 순위");
    check(perm, "순위는 모든 노드를 한 번씩 포함 (빠지거나 겹치는 노드 없음)");
    check(desc, "점수가 높은 노드부터 정렬됨");
}

static void test_distribution(void)
{
    printf("[2] 분포: 노드 3개에 URL %d개\n", NKEYS);
    const char *names[] = { "node1", "node2", "node3" };
    uint64_t seeds[3];
    for (int i = 0; i < 3; i++)
        seeds[i] = rendezvous_seed(names[i]);

    long cnt[3] = { 0 };
    char key[96];
    for (int k = 0; k < NKEYS; k++) {
        key_of(key, sizeof key, k);
        cnt[owner_of(seeds, 3, key)]++;
    }
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        double pct = 100.0 * (double)cnt[i] / NKEYS;
        printf("      %s: %6ld개 (%.2f%%)\n", names[i], cnt[i], pct);
        if (pct < 32.0 || pct > 34.67)
            ok = 0;
    }
    check(ok, "세 노드 모두 33.3%%에 가까움 (허용 범위 32.0~34.7%%)");
}

static void test_remove(void)
{
    printf("[3] 노드 하나 제거: node1·node2·node3 → node1·node3 (node2 장애)\n");
    const char *n3[] = { "node1", "node2", "node3" };
    const char *n2[] = { "node1", "node3" };
    uint64_t s3[3], s2[2];
    for (int i = 0; i < 3; i++) s3[i] = rendezvous_seed(n3[i]);
    for (int i = 0; i < 2; i++) s2[i] = rendezvous_seed(n2[i]);

    long moved = 0, bad_stay = 0, bad_dest = 0, modn_moved = 0;
    char key[96];
    for (int k = 0; k < NKEYS; k++) {
        key_of(key, sizeof key, k);
        int order3[3];
        rendezvous_rank(s3, 3, key, order3, NULL);
        const char *before = n3[order3[0]];
        const char *after  = n2[owner_of(s2, 2, key)];

        if (strcmp(before, "node2") != 0) {
            if (strcmp(before, after) != 0)
                bad_stay++;                 /* node2 담당이 아니었는데 옮겨짐 → 있으면 안 됨 */
        } else {
            moved++;
            if (strcmp(n3[order3[1]], after) != 0)
                bad_dest++;                 /* node2의 몫이 2순위가 아닌 곳으로 감 → 있으면 안 됨 */
        }
        if (strcmp(n3[modn_owner(key, 3)], n2[modn_owner(key, 2)]) != 0)
            modn_moved++;
    }
    check(bad_stay == 0, "node2가 담당이 아니던 URL은 하나도 옮겨지지 않음 (위반 %ld개)", bad_stay);
    check(bad_dest == 0, "node2가 담당하던 URL은 모두 각자의 2순위 노드로 옮겨짐 (위반 %ld개)", bad_dest);
    printf("      담당이 바뀐 URL 비율   랑데부 해싱 %.1f%%   |   hash %% N  %.1f%%\n",
           100.0 * moved / NKEYS, 100.0 * modn_moved / NKEYS);
    check(modn_moved > moved, "hash %% N은 꼭 옮겨야 하는 것보다 훨씬 많이 옮김 (이론값 33.3%% 대 66.7%%)");
}

static void test_add(void)
{
    printf("[4] 노드 하나 추가: node1·node2·node3 → + node4\n");
    const char *n3[] = { "node1", "node2", "node3" };
    const char *n4[] = { "node1", "node2", "node3", "node4" };
    uint64_t s3[3], s4[4];
    for (int i = 0; i < 3; i++) s3[i] = rendezvous_seed(n3[i]);
    for (int i = 0; i < 4; i++) s4[i] = rendezvous_seed(n4[i]);

    long moved = 0, bad = 0, modn_moved = 0;
    char key[96];
    for (int k = 0; k < NKEYS; k++) {
        key_of(key, sizeof key, k);
        const char *before = n3[owner_of(s3, 3, key)];
        const char *after  = n4[owner_of(s4, 4, key)];
        if (strcmp(before, after) != 0) {
            moved++;
            if (strcmp(after, "node4") != 0)
                bad++;                      /* 옮겨졌다면 반드시 새 노드로 가야 함 */
        }
        if (strcmp(n3[modn_owner(key, 3)], n4[modn_owner(key, 4)]) != 0)
            modn_moved++;
    }
    check(bad == 0, "옮겨진 URL은 모두 새 노드(node4)로 감 (위반 %ld개)", bad);
    double pct = 100.0 * moved / NKEYS;
    printf("      담당이 바뀐 URL 비율   랑데부 해싱 %.1f%%   |   hash %% N  %.1f%%\n",
           pct, 100.0 * modn_moved / NKEYS);
    check(pct > 24.0 && pct < 26.0, "랑데부 해싱은 새 노드의 몫(1/4)만큼만 옮김 (이론값 25%% 대 75%%)");
}

static void test_real_objects(void)
{
    printf("[5] 실험용 객체 200개(obj/0000~0199.bin)의 담당 노드 (node1·node2·node3)\n");
    const char *names[] = { "node1", "node2", "node3" };
    uint64_t seeds[3];
    for (int i = 0; i < 3; i++)
        seeds[i] = rendezvous_seed(names[i]);
    int cnt[3] = { 0 };
    char key[96];
    for (int k = 0; k < 200; k++) {
        snprintf(key, sizeof key, "http://127.0.0.1:9000/obj/%04d.bin", k);
        cnt[owner_of(seeds, 3, key)]++;
    }
    printf("      node1 %d개, node2 %d개, node3 %d개\n", cnt[0], cnt[1], cnt[2]);
    check(cnt[0] + cnt[1] + cnt[2] == 200 && cnt[0] > 0 && cnt[1] > 0 && cnt[2] > 0,
          "세 노드가 모두 일부를 맡음");
}

/* ------------------------------------------------------------------ */

static int cmd_rank(int argc, char *argv[])
{
    const char *conf = "conf/cluster.conf";
    int i = 0;
    if (i + 1 < argc && strcmp(argv[i], "-c") == 0) {
        conf = argv[i + 1];
        i += 2;
    }
    if (i >= argc) {
        fprintf(stderr, "사용법: rendezvous_test rank [-c 노드목록] URL...\n");
        return 2;
    }

    char err[256];
    if (router_load(conf, NULL, err, sizeof err) < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    for (; i < argc; i++) {
        int order[ROUTER_MAX_NODES];
        uint64_t sc[ROUTER_MAX_NODES];
        int n = router_rank(argv[i], order, sc);
        printf("%s\n", argv[i]);
        for (int r = 0; r < n; r++)
            printf("  %d순위 %-8s 점수 %016llx%s\n", r + 1, router_node(order[r])->name,
                   (unsigned long long)sc[r], r == 0 ? "  ← 담당" : (r == 1 ? "  ← 담당 장애 시 대체" : ""));
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "rank") == 0)
        return cmd_rank(argc - 2, argv + 2);

    printf("랑데부 해싱 점검\n");
    test_basic();
    test_distribution();
    test_remove();
    test_add();
    test_real_objects();
    printf("\n결과: %s\n", g_failures == 0 ? "모두 통과" : "실패 있음");
    return g_failures == 0 ? 0 : 1;
}

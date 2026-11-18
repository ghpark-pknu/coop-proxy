/*
 * lockstat.h — 락 경합 계측 (5단계)
 *
 * 락을 잡을 때 "다른 스레드가 이미 잡고 있어서 기다려야 했는가"를 센다.
 *   1) pthread_mutex_trylock: 기다리지 않고 바로 잡아 본다. 비어 있으면 성공 → 경합 없음
 *   2) 실패했다면 다른 스레드가 잡고 있다는 뜻 → 경합 1회.
 *      기다린 시간을 재면서 pthread_mutex_lock으로 잡는다
 *
 * 카운터는 락을 잡은 "뒤에" 고치므로, 그 락이 카운터도 보호한다 (원자 연산이 필요 없다).
 * 경합이 없을 때 더 드는 비용은 없다: pthread_mutex_lock 대신 trylock을 한 번 부를 뿐이다.
 * 경합이 있을 때만 시계를 두 번 읽는다 (clock_gettime은 vDSO라서 시스템 호출 없이 수십 ns).
 *
 * 읽을 때는 같은 락을 잡고 구조체를 통째로 복사한다 (그때는 세지 않는다: 관제 화면이
 * 1초마다 통계를 읽는 것까지 세면, 측정하려는 값에 측정 도구의 몫이 섞인다).
 */
#ifndef LOCKSTAT_H
#define LOCKSTAT_H

#include <pthread.h>
#include <time.h>

struct lock_stats {
    unsigned long      acquires;    /* 락을 잡은 횟수 */
    unsigned long      contended;   /* 그중 다른 스레드가 잡고 있어서 기다린 횟수 */
    unsigned long long wait_ns;     /* 기다린 시간의 합 (나노초) */
};

/* 락을 잡으면서 경합을 기록한다. ls는 m이 보호하는 구조체여야 한다 */
static inline void lockstat_lock(pthread_mutex_t *m, struct lock_stats *ls)
{
    if (pthread_mutex_trylock(m) != 0) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        pthread_mutex_lock(m);
        clock_gettime(CLOCK_MONOTONIC, &b);
        ls->contended++;
        ls->wait_ns += (unsigned long long)((long long)(b.tv_sec - a.tv_sec) * 1000000000LL
                                            + (b.tv_nsec - a.tv_nsec));
    }
    ls->acquires++;
}

/* sum += x */
static inline void lockstat_add(struct lock_stats *sum, const struct lock_stats *x)
{
    sum->acquires  += x->acquires;
    sum->contended += x->contended;
    sum->wait_ns   += x->wait_ns;
}

#endif /* LOCKSTAT_H */

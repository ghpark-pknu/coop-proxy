/*
 * stats.h — 통계 수집기
 *
 * 요청 하나가 끝날 때마다 결과를 기록한다. 캐시와는 별도의 락으로 보호한다.
 * (캐시 락과 통계 락을 나누면, 통계를 기록하는 동안 캐시 조회가 막히지 않는다)
 */
#ifndef STATS_H
#define STATS_H

/* 요청 하나의 처리 결과 분류 */
enum outcome {
    OUT_HIT,        /* 캐시에서 바로 응답 */
    OUT_VALIDATED,  /* 검증 주기가 지나 원서버에 확인 → 304 → 캐시로 응답 */
    OUT_STALE,      /* 검증하려 했지만 원서버 연결 실패 → 오래된 캐시로 응답 */
    OUT_MISS,       /* 캐시에 없음 → 원서버에서 받아 응답 (저장 가능하면 저장) */
    OUT_REFRESH,    /* 검증 결과 내용이 바뀜(200) → 새 응답으로 교체 */
    OUT_BLOCKED,    /* 차단 목록 → 403 */
    OUT_ERROR,      /* 그 밖의 오류 응답 (400, 501, 502, 504) */
    OUT_COUNT
};

struct stats_snapshot {
    unsigned long      count[OUT_COUNT];    /* 결과별 요청 수 */
    unsigned long      requests;            /* 전체 요청 수 */
    unsigned long      origin_requests;     /* 원서버에 실제로 보낸 요청 수 (조건부 요청 포함) */
    unsigned long      forwards;            /* 다른 노드로 전달한 수 (3단계) */
    unsigned long long bytes;               /* 클라이언트에 보낸 바이트 합 */
    unsigned long long sum_ms;              /* 응답 시간 합 (평균 계산용) */
    long long          max_ms;              /* 가장 오래 걸린 응답 시간 */
};

const char *outcome_name(enum outcome o);

/* origin_requests: 이 요청을 처리하며 원서버에 보낸 요청 수 (0 또는 1) */
void stats_record(enum outcome o, int origin_requests, long long bytes, long long ms);

void stats_get(struct stats_snapshot *out);

/* 히트율 = 캐시로 응답한 요청 / 캐시를 거친 요청 (오류·차단 제외). 요청이 없으면 0 */
double stats_hit_ratio(const struct stats_snapshot *s);

#endif /* STATS_H */

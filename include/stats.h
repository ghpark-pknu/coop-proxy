/*
 * stats.h — 통계 수집기
 *
 * 요청 하나가 끝날 때마다 결과를 기록한다. 캐시와는 별도의 락으로 보호한다.
 * (캐시 락과 통계 락을 나누면, 통계를 기록하는 동안 캐시 조회가 막히지 않는다)
 *
 * 클러스터 전체를 볼 때: 클라이언트 요청 하나는 "직접 처리한 노드"에서 정확히 한 번
 * HIT·MISS 등으로 세어지고, 넘겨준 노드에서는 FORWARD로 세어진다.
 * 그래서 노드들의 HIT·MISS를 더하면 클러스터 전체의 히트율이 된다.
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
    OUT_FORWARD,    /* 담당 노드(또는 대체 노드)로 넘겨 받은 응답을 중계 */
    OUT_BLOCKED,    /* 차단 목록 → 403 */
    OUT_ERROR,      /* 그 밖의 오류 응답 (400, 501, 502, 504) */
    OUT_COUNT
};

/* 요청 하나의 기록 */
struct stats_event {
    enum outcome outcome;
    int          origin_requests;   /* 이 요청 때문에 원서버에 보낸 요청 수 (0 또는 1) */
    int          peer_failures;     /* 연결하지 못한 다른 노드 수 (0~2) */
    int          from_peer;         /* 다른 노드가 넘겨준 요청이면 1 */
    long long    bytes;             /* 클라이언트에 보낸 바이트 */
    long long    ms;                /* 처리 시간 */
};

struct stats_snapshot {
    unsigned long      count[OUT_COUNT];    /* 결과별 요청 수 */
    unsigned long      requests;            /* 이 노드가 받은 전체 요청 수 */
    unsigned long      origin_requests;     /* 원서버에 실제로 보낸 요청 수 (조건부 요청 포함) */
    unsigned long      peer_failures;       /* 다른 노드 연결 실패 횟수 */
    unsigned long      from_peer;           /* 다른 노드가 넘겨준 요청 수 */
    unsigned long long bytes;               /* 클라이언트에 보낸 바이트 합 */
    unsigned long long sum_ms;              /* 응답 시간 합 (평균 계산용) */
    long long          max_ms;              /* 가장 오래 걸린 응답 시간 */
};

const char *outcome_name(enum outcome o);

void stats_record(const struct stats_event *e);
void stats_get(struct stats_snapshot *out);

/* 히트율 = 캐시로 응답한 요청 / 캐시를 거친 요청 (FORWARD·오류·차단 제외). 요청이 없으면 0 */
double stats_hit_ratio(const struct stats_snapshot *s);

#endif /* STATS_H */

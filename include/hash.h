/*
 * hash.h — 해시 함수 (캐시 버킷 선택과 랑데부 해싱에서 함께 사용)
 *
 * 두 함수 모두 짧아서 호출 비용을 없애려고 static inline으로 헤더에 둔다.
 */
#ifndef HASH_H
#define HASH_H

#include <stdint.h>

/* FNV-1a (64비트): 문자열 → 64비트 값.
 * 바이트마다 "XOR 후 정해진 소수를 곱하기"를 반복한다 */
static inline uint64_t hash_fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;        /* FNV offset basis */
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;                  /* FNV prime */
    }
    return h;
}

/* 64비트 값을 골고루 뒤섞는다 (MurmurHash3의 마무리 함수 fmix64).
 * 입력의 비트 하나만 달라도 출력 비트의 약 절반이 바뀐다 (눈사태 효과).
 * 서로 다른 입력은 서로 다른 출력으로 간다 (일대일 함수). */
static inline uint64_t hash_mix64(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

#endif /* HASH_H */

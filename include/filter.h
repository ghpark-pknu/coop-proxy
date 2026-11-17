/*
 * filter.h — 사이트 차단 필터
 *
 * 차단 목록 파일 형식 (conf/blocklist.txt)
 *   - 한 줄에 호스트 하나, '#' 뒤는 주석, 빈 줄은 무시
 *   - "example.com"을 적으면 example.com과 그 하위 도메인(www.example.com 등)을 모두 차단
 *   - 대소문자를 구별하지 않는다
 *
 * 프로그램 시작 때 한 번 읽고 이후로는 읽기만 하므로 락이 필요 없다.
 */
#ifndef FILTER_H
#define FILTER_H

#include <stddef.h>

/* 성공 0, 파일을 열 수 없으면 -1 */
int    filter_load(const char *path);
int    filter_is_blocked(const char *host);
size_t filter_count(void);
void   filter_free(void);

#endif /* FILTER_H */

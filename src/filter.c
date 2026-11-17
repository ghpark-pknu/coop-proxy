/*
 * filter.c — 사이트 차단 필터
 */
#include "filter.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char  **g_hosts;     /* 소문자로 바꿔 저장한 차단 호스트 목록 */
static size_t  g_count;

int filter_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return -1;

    char line[512];
    while (fgets(line, sizeof line, fp) != NULL) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';                       /* 주석 제거 */

        char *s = line;
        while (isspace((unsigned char)*s))
            s++;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1]))
            *--e = '\0';
        if (*s == '\0')
            continue;                           /* 빈 줄 */

        for (char *p = s; *p; p++)
            *p = (char)tolower((unsigned char)*p);

        char **grown = realloc(g_hosts, (g_count + 1) * sizeof *grown);
        char  *copy  = strdup(s);
        if (grown == NULL || copy == NULL) {
            free(copy);
            if (grown)
                g_hosts = grown;
            fclose(fp);
            return -1;
        }
        g_hosts = grown;
        g_hosts[g_count++] = copy;
    }
    fclose(fp);
    return 0;
}

int filter_is_blocked(const char *host)
{
    size_t hlen = strlen(host);
    for (size_t i = 0; i < g_count; i++) {
        size_t blen = strlen(g_hosts[i]);
        if (hlen == blen && strcasecmp(host, g_hosts[i]) == 0)
            return 1;                           /* 정확히 같음: example.com */
        if (hlen > blen && host[hlen - blen - 1] == '.'
            && strcasecmp(host + hlen - blen, g_hosts[i]) == 0)
            return 1;                           /* 하위 도메인: www.example.com */
    }
    return 0;
}

size_t filter_count(void)
{
    return g_count;
}

void filter_free(void)
{
    for (size_t i = 0; i < g_count; i++)
        free(g_hosts[i]);
    free(g_hosts);
    g_hosts = NULL;
    g_count = 0;
}

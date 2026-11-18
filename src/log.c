/*
 * log.c — 실행 기록
 *
 * 여러 스레드가 동시에 기록하므로, 한 줄을 지역 버퍼에 완성한 뒤
 * fputs() 한 번으로 내보낸다. stdio 함수는 호출 하나 단위로 FILE에 락을
 * 걸기 때문에, 이렇게 하면 다른 스레드의 줄과 섞이지 않는다.
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static char g_node[32] = "node";
static int  g_quiet;

void log_init(const char *node, int quiet)
{
    snprintf(g_node, sizeof g_node, "%s", node);
    g_quiet = quiet;
}

/* "2026-11-17 17:02:11.204" 형식의 현재 시각 */
static void timestamp(char *out, size_t cap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);       /* localtime()은 내부 정적 버퍼를 써서 스레드에 안전하지 않다 */
    size_t n = strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(out + n, cap - n, ".%03ld", ts.tv_nsec / 1000000);
}

void log_request(const struct req_log *r)
{
    if (g_quiet)
        return;

    char ts[32];
    timestamp(ts, sizeof ts);

    char status[12] = "-";      /* int 최댓값 10자리 + 부호 + '\0' */
    if (r->status > 0)
        snprintf(status, sizeof status, "%d", r->status);

    char result[48];
    if (r->note)
        snprintf(result, sizeof result, "%s(%s)", outcome_name(r->outcome), r->note);
    else
        snprintf(result, sizeof result, "%s", outcome_name(r->outcome));

    char line[1024];
    snprintf(line, sizeof line, "%s [%s] %s %s %.300s %s %lldB %.1fms %s%s%s\n",
             ts, g_node, r->client, r->method, r->target, status, r->bytes, (double)r->us / 1000.0,
             result, r->route[0] ? " " : "", r->route);
    fputs(line, stdout);
}

void log_info(const char *fmt, ...)
{
    char ts[32];
    timestamp(ts, sizeof ts);

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    char line[640];
    snprintf(line, sizeof line, "%s [%s] %s\n", ts, g_node, msg);
    fputs(line, stdout);
}

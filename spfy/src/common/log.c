#include "log.h"

#include <stdarg.h>

void spfy_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("spfy: warn: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void spfy_log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("spfy: error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#ifdef SPFY_TRACE
static FILE *g_trace_sink = NULL;

void spfy_trace_set_sink(FILE *fp) { g_trace_sink = fp; }

void spfy_trace_event(const char *stage, const char *json_payload)
{
    if (!g_trace_sink) return;
    fprintf(g_trace_sink, "{\"stage\":\"%s\",\"data\":%s}\n",
            stage, json_payload);
}
#endif

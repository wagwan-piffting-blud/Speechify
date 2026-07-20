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

/* Always compiled (see log.h): the call-site macros gate whether these are
 * ever reached, not whether they exist. Each event is flushed immediately so
 * a downstream reader (the viz SSE relay) sees lines as synthesis runs. */
static FILE *g_trace_sink = NULL;

void spfy_trace_set_sink_impl(FILE *fp) { g_trace_sink = fp; }

void spfy_trace_event_impl(const char *stage, const char *json_payload)
{
    if (!g_trace_sink) return;
    fprintf(g_trace_sink, "{\"stage\":\"%s\",\"data\":%s}\n",
            stage, json_payload);
    fflush(g_trace_sink);
}

void spfy_trace_eventf_impl(const char *stage, const char *fmt, ...)
{
    if (!g_trace_sink) return;
    fprintf(g_trace_sink, "{\"stage\":\"%s\",\"data\":", stage);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_trace_sink, fmt, ap);
    va_end(ap);
    fputs("}\n", g_trace_sink);   /* close the outer object the header opened */
    fflush(g_trace_sink);
}

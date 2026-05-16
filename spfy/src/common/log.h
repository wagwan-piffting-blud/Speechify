#ifndef SPFY_COMMON_LOG_H
#define SPFY_COMMON_LOG_H

/* Lightweight logging. Trace builds (-DSPFY_TRACE=1) emit JSONL via
 * spfy_trace(); release builds compile it out. */

#include <stdio.h>

void spfy_log_warn(const char *fmt, ...);
void spfy_log_err (const char *fmt, ...);

#ifdef SPFY_TRACE
void spfy_trace_set_sink(FILE *fp);
void spfy_trace_event(const char *stage, const char *json_payload);
#else
#define spfy_trace_set_sink(fp)         ((void)(fp))
#define spfy_trace_event(stage, json)   do { (void)(stage); (void)(json); } while (0)
#endif

#endif

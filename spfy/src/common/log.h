#ifndef SPFY_COMMON_LOG_H
#define SPFY_COMMON_LOG_H

/* Lightweight logging. Trace builds (-DSPFY_TRACE=1) emit JSONL via
 * spfy_trace(); release builds compile it out. */

#include <stdio.h>

void spfy_log_warn(const char *fmt, ...);
void spfy_log_err (const char *fmt, ...);

/* Trace sink primitives. These are ALWAYS compiled into spfy_common, even
 * in non-trace builds, so a translation unit built with -DSPFY_TRACE=1
 * (e.g. the spfy_synth_trace CLI) can link against them while spfy_common
 * itself is built without SPFY_TRACE. Non-trace call sites never reference
 * these symbols: the public spfy_trace_* macros below expand to no-ops, so
 * the emit calls — AND the argument expressions that build their payloads —
 * vanish entirely (zero runtime cost, byte-identical output). Distinct
 * `_impl` names avoid the macro colliding with the prototype. */
void spfy_trace_set_sink_impl(FILE *fp);
void spfy_trace_event_impl(const char *stage, const char *json_payload);
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void spfy_trace_eventf_impl(const char *stage, const char *fmt, ...);

#ifdef SPFY_TRACE
#define spfy_trace_set_sink(fp)         spfy_trace_set_sink_impl(fp)
#define spfy_trace_event(stage, json)   spfy_trace_event_impl((stage), (json))
#define spfy_trace_eventf(stage, ...)   spfy_trace_eventf_impl((stage), __VA_ARGS__)
#else
#define spfy_trace_set_sink(fp)         ((void)(fp))
#define spfy_trace_event(stage, json)   do { (void)(stage); (void)(json); } while (0)
#define spfy_trace_eventf(stage, ...)   do { (void)(stage); } while (0)
#endif

#endif

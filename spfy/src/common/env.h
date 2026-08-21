/* Cached getenv. */

#ifndef SPFY_COMMON_ENV_H
#define SPFY_COMMON_ENV_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *spfy_env(const char *name);

int spfy_env_set(const char *name);

/* Stream for a diagnostic dump named by env var, or NULL when it is unset.
 * A PATH value gives a fully-buffered file; "1"/"-"/"stderr" gives stderr,
 * which is UNBUFFERED and therefore one syscall per line. Use a path for
 * anything that fires inside a DP loop. Cached by key pointer. */
FILE *spfy_dump_stream(const char *name);

/* Drop every cached answer. */
void spfy_env_reset(void);

#ifdef __cplusplus
}
#endif

#endif

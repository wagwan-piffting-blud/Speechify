/* Cached getenv. */

#ifndef SPFY_COMMON_ENV_H
#define SPFY_COMMON_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

const char *spfy_env(const char *name);

int spfy_env_set(const char *name);

/* Drop every cached answer. */
void spfy_env_reset(void);

#ifdef __cplusplus
}
#endif

#endif

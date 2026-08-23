/* In-house front-end - text → tagged-output assembler. */

#ifndef SPFY_FE_INTERNAL_H
#define SPFY_FE_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert raw input text to tagged-text in `out`. */
int spfy_fe_internal_text_to_tagged(const char *text,
                                     char *out, size_t out_n);

#ifdef __cplusplus
}
#endif

#endif

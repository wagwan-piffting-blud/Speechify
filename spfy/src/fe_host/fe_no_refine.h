#ifndef SPFY_FE_NO_REFINE_H
#define SPFY_FE_NO_REFINE_H

#include <stddef.h>

/* Lookup: returns 1 if the (word, syl_idx) pair is in the engine- derived
 * no-refinement override table. */
int spfy_fe_should_skip_refinement(const char *word, int syl_idx);

#endif

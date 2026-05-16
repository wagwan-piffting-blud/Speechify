#ifndef SPFY_FE_NO_REFINE_H
#define SPFY_FE_NO_REFINE_H

#include <stddef.h>

/* Lookup: returns 1 if the (word, syl_idx) pair is in the engine-
 * derived no-refinement override table.
 *
 * Generated from engine fe_tree captures (500 phrases) by
 * spfy/tools/no_refine_codegen.py — do not edit by hand. */
int spfy_fe_should_skip_refinement(const char *word, int syl_idx);

#endif /* SPFY_FE_NO_REFINE_H */

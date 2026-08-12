/* syl_span.c -- see syl_span.h for why this exists. */
#include "syl_span.h"
#include "env.h"

int spfy_syl_merge_enabled(void)
{
    static int on = -1;
    if (on < 0) on = (spfy_env("SPFY_FE_LIAISON_LEGACY") == NULL);
    return on;
}

int spfy_syl_continues_prev(const spfy_fe_utt_t *utt, uint32_t fe_sidx)
{
    if (!utt || !utt->syl_cont_prev) return 0;
    if (!spfy_syl_merge_enabled()) return 0;
    if (fe_sidx == 0 || fe_sidx >= utt->n_syls) return 0;
    return utt->syl_cont_prev[fe_sidx] != 0;
}

uint32_t spfy_syl_effective(const spfy_fe_utt_t *utt, uint32_t fe_sidx)
{
    if (!utt || !utt->syl_cont_prev || fe_sidx >= utt->n_syls) return fe_sidx;
    if (!spfy_syl_merge_enabled()) return fe_sidx;
    /* Walk back over a chain of continuations. */
    uint32_t cur = fe_sidx;
    while (cur > 0 && utt->syl_cont_prev[cur]) --cur;
    return cur;
}


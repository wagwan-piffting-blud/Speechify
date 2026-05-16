/* F0 (pitch) cost component. */

#include "cost.h"

float spfy_cost_f0(uint32_t stored_f0,
                   float f0_pred_mean,
                   float f0_pred_scale,
                   float abs_f0_weight,
                   float missing_f0_cost)
{
    /* Engine gates on stored_f0 == 0 (unvoiced/silence) BEFORE computing
     * the squared-error term. See DLL_ANALYSIS "F0 scoring" step 4. */
    if (stored_f0 == 0u) return missing_f0_cost;

    long double diff   = (long double)stored_f0 - (long double)f0_pred_mean;
    long double scaled = (long double)f0_pred_scale * diff;
    long double sq     = scaled * scaled;
    long double cost   = sq * (long double)abs_f0_weight;
    return (float)cost;
}

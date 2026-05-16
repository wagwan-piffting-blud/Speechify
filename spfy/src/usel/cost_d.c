/* Duration cost component. */

#include "cost.h"

float spfy_cost_d(uint32_t stored_dur,
                  float durt_pred_mean,
                  float durt_pred_scale,
                  float dur_weight)
{
    long double diff   = (long double)stored_dur - (long double)durt_pred_mean;
    long double scaled = (long double)durt_pred_scale * diff;
    long double sq     = scaled * scaled;
    long double cost   = sq * (long double)dur_weight;
    return (float)cost;
}

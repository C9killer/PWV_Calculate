#include "wk_feature_adapter.h"

#include <math.h>

#include "wk_feature_adapter_values.h"

void wk_apply_feature_adapter(
    double features[WK_COMPACT_BASE_FEATURE_COUNT]
)
{
#if WK_CAUSAL_ADAPTER_FITTED
    size_t index;
    for (index = 0u; index < WK_COMPACT_BASE_FEATURE_COUNT; ++index) {
        const double adapted = features[index] * wk_feature_adapter_scale[index]
            + wk_feature_adapter_offset[index];
        if (isfinite(adapted)) {
            features[index] = adapted < wk_feature_adapter_lower[index]
                ? wk_feature_adapter_lower[index]
                : (adapted > wk_feature_adapter_upper[index]
                    ? wk_feature_adapter_upper[index] : adapted);
        }
    }
#else
    (void)features;
#endif
}

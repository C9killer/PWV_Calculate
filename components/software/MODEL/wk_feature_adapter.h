#ifndef WK_FEATURE_ADAPTER_H
#define WK_FEATURE_ADAPTER_H

#include "wk_required_base_features.h"

/** Map causal 250 Hz features into the frozen model's training domain. */
void wk_apply_feature_adapter(
    double features[WK_COMPACT_BASE_FEATURE_COUNT]
);

#endif

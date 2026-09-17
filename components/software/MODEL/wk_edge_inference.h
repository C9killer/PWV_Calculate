#ifndef WK_EDGE_INFERENCE_H
#define WK_EDGE_INFERENCE_H

#include <stdbool.h>
#include "wk_esp32_partition.h"

typedef struct {
    wk_flash_models_t models;
    int abs_r_id, abs_c_id, diff_r_id, diff_c_id;
    int abs_sbp_id, abs_dbp_id, diff_sbp_id, diff_dbp_id;
    bool has_previous;
    bool difference_ready;
    /* 7.4 KiB persistent state. Keep this in normal RAM; never use task stack. */
    double previous[WK_COMPACT_BASE_FEATURE_COUNT];
    double difference_input[4u * WK_COMPACT_BASE_FEATURE_COUNT];
} wk_edge_state_t;

typedef struct { double r_absolute, c_absolute, r_difference, c_difference; } wk_rc_model_output_t;
typedef struct { double sbp_absolute, dbp_absolute, sbp_difference, dbp_difference; } wk_bp_model_output_t;

esp_err_t wk_edge_state_init(wk_edge_state_t *state);
esp_err_t wk_edge_predict_rc(wk_edge_state_t *state,
    const double features[WK_COMPACT_BASE_FEATURE_COUNT], wk_rc_model_output_t *output);
esp_err_t wk_edge_predict_bp(wk_edge_state_t *state,
    const double features[WK_COMPACT_BASE_FEATURE_COUNT], const double mechanism[6],
    wk_bp_model_output_t *output);

#endif

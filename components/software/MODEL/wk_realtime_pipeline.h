#ifndef WK_REALTIME_PIPELINE_H
#define WK_REALTIME_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wk_causal_calibration.h"
#include "wk_edge_inference.h"
#include "wk_feature_extractor.h"
#include "wk_signal_pipeline.h"

typedef enum {
    WK_RT_NO_NEW_WINDOW = 0,
    WK_RT_OUTPUT_READY,
    WK_RT_SIGNAL_REJECTED,
    WK_RT_MODEL_ERROR,
    WK_RT_PHYSIOLOGY_ERROR
} wk_realtime_status_t;

typedef struct {
    wk_realtime_status_t status;
    wk_signal_quality_t signal_quality;
    uint64_t sample_index;
    double sbp;
    double dbp;
    double resistance;
    double compliance;
    double stroke_volume;
    double inverse_pat;
    double wk_sbp;
    double wk_dbp;
    bool used_reference;
} wk_realtime_output_t;

typedef struct {
    wk_signal_stream_t stream;
    wk_feature_workspace_t feature_workspace;
    wk_edge_state_t edge;
    wk_online_parameter_t resistance_filter;
    wk_online_parameter_t compliance_filter;
    wk_online_bp_t sbp_filter;
    wk_online_bp_t dbp_filter;
    float ecg_window[WK_WINDOW_SAMPLES];
    float finger_window[WK_WINDOW_SAMPLES];
    float wrist_window[WK_WINDOW_SAMPLES];
    double features[WK_COMPACT_BASE_FEATURE_COUNT];
    uint64_t sample_index;
    bool reference_pending;
    double reference_sbp;
    double reference_dbp;
} wk_realtime_state_t;

/** Initialize all streaming, calibration, physiological and model states. */
esp_err_t wk_realtime_init(
    wk_realtime_state_t *state,
    float finger_polarity,
    float wrist_polarity
);

/**
 * Attach one synchronized cuff/reference BP observation to the next completed
 * 1 Hz inference window. Repeated calls may supply the initial ten anchors.
 */
esp_err_t wk_realtime_set_reference(
    wk_realtime_state_t *state,
    double reference_sbp,
    double reference_dbp
);

/**
 * Push one synchronized 250 Hz ECG/finger-PPG/wrist-PPG sample. The function
 * returns ESP_OK for normal operation; inspect output->status to determine
 * whether a new BP result was produced.
 */
esp_err_t wk_realtime_push_sample(
    wk_realtime_state_t *state,
    float ecg,
    float finger_ppg,
    float wrist_ppg,
    wk_realtime_output_t *output
);

const char *wk_realtime_status_name(wk_realtime_status_t status);

#endif

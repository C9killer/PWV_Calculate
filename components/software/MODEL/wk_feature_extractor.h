#ifndef WK_FEATURE_EXTRACTOR_H
#define WK_FEATURE_EXTRACTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wk_required_base_features.h"
#include "wk_signal_pipeline.h"

#define WK_MAX_BEATS 24u
#define WK_CYCLE_POINTS 80u
#define WK_MAX_FFT_BINS 112u

typedef enum {
    WK_SIGNAL_OK = 0,
    WK_SIGNAL_NONFINITE,
    WK_SIGNAL_FLAT,
    WK_SIGNAL_TOO_FEW_PPG_BEATS,
    WK_SIGNAL_TOO_FEW_ECG_BEATS,
    WK_SIGNAL_RATE_OUT_OF_RANGE,
    WK_SIGNAL_IRREGULAR_RHYTHM,
    WK_SIGNAL_POOR_WRIST_CONTACT,
    WK_SIGNAL_FEATURE_FAILURE
} wk_signal_status_t;

typedef struct {
    wk_signal_status_t status;
    float quality_score;
    float heart_rate_bpm;
    float rr_cv;
    float finger_wrist_correlation;
    uint16_t ppg_peak_count;
    uint16_t ecg_peak_count;
    uint16_t usable_cycle_count;
} wk_signal_quality_t;

typedef struct {
    float ppg_normalized[WK_WINDOW_SAMPLES];
    float wrist_normalized[WK_WINDOW_SAMPLES];
    float ecg_normalized[WK_WINDOW_SAMPLES];
    float derivatives[5][WK_WINDOW_SAMPLES];
    float peak_cycle[5][WK_CYCLE_POINTS];
    float valley_cycle[5][WK_CYCLE_POINTS];
    float fft_magnitude[WK_MAX_FFT_BINS];
    uint16_t ppg_peaks[WK_MAX_BEATS];
    uint16_t ppg_valleys[WK_MAX_BEATS + 1u];
    uint16_t ecg_peaks[WK_MAX_BEATS];
    uint16_t wrist_peaks[WK_MAX_BEATS];
} wk_feature_workspace_t;

/**
 * Extract the exact 185-element compact input layout expected by the packed
 * MIMIC-III models. Algorithms are causal at window level: only the current
 * and previous samples in the rolling 5 s buffer are used.
 */
bool wk_extract_features_250hz(
    const float ecg[WK_WINDOW_SAMPLES],
    const float finger_ppg[WK_WINDOW_SAMPLES],
    const float wrist_ppg[WK_WINDOW_SAMPLES],
    wk_feature_workspace_t *workspace,
    double features[WK_COMPACT_BASE_FEATURE_COUNT],
    wk_signal_quality_t *quality
);

const char *wk_signal_status_name(wk_signal_status_t status);

#endif

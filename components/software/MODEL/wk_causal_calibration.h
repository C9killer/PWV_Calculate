#ifndef WK_CAUSAL_CALIBRATION_H
#define WK_CAUSAL_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define WK_DAAF_HISTORY 21u
#define WK_REFERENCE_CALIBRATION_POINTS 10u

typedef struct {
    bool has_previous;
    double previous_driver;
    double previous_absolute;
    double previous_daaf;
    double static_history[3];
    size_t static_count;
    size_t static_write;
    double absolute_level[WK_DAAF_HISTORY];
    double calibrated_level[WK_DAAF_HISTORY];
    size_t level_count;
    size_t level_write;
    double driver_difference[WK_DAAF_HISTORY];
    double prediction_difference[WK_DAAF_HISTORY];
    size_t difference_count;
    size_t difference_write;
} wk_causal_daaf_t;

typedef struct {
    wk_causal_daaf_t daaf;
    bool kalman_initialized;
    double estimate;
    double variance;
    double process_variance;
    double measurement_variance;
    double bias_ratio_sum;
    size_t bias_count;
    bool has_previous_reference;
    double previous_reference;
} wk_online_parameter_t;

typedef struct {
    bool initialized;
    double estimate;
    double variance;
    double process_variance;
    double measurement_variance;
    bool has_previous_reference;
    double previous_reference;
    size_t reference_count;
    double absolute_bias_sum;
} wk_online_bp_t;

void wk_online_parameter_init(
    wk_online_parameter_t *state,
    double process_variance,
    double measurement_variance
);

/**
 * Causal replacement for the offline DAAF + proportional calibration +
 * absolute/difference Kalman chain. The optional reference is an inverse-WK
 * R or C value derived from a contemporaneous cuff measurement.
 */
double wk_online_parameter_update(
    wk_online_parameter_t *state,
    double absolute_prediction,
    double difference_prediction,
    double driver_sv,
    const double *reference
);

void wk_online_bp_init(
    wk_online_bp_t *state,
    double process_variance,
    double measurement_variance
);

/** Forward-only final BP fusion; reference may be NULL after initialization. */
double wk_online_bp_update(
    wk_online_bp_t *state,
    double absolute_prediction,
    double difference_prediction,
    const double *reference
);

/**
 * Estimate effective R and C from one reference SBP/DBP observation using the
 * same two-element WK forward equation as the embedded estimator.
 */
esp_err_t wk_inverse_wk_reference(
    double reference_sbp,
    double reference_dbp,
    double cardiac_cycle,
    double systolic_duration,
    double stroke_volume,
    double *resistance,
    double *compliance
);

#endif

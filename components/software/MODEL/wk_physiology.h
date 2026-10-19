#ifndef WK_PHYSIOLOGY_H
#define WK_PHYSIOLOGY_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    bool initialized;
    double estimate;
    double variance;
    double process_variance;
    double measurement_variance;
} wk_kalman_state_t;

/** Exact two-element WK calculation used by the PC pipeline. */
esp_err_t wk_two_element_bp(
    double resistance,
    double compliance,
    double cardiac_cycle,
    double systolic_duration,
    double stroke_volume,
    unsigned int cycles,
    double *sbp,
    double *dbp
);

/** Initialize the paper's forward absolute/difference Kalman recursion. */
esp_err_t wk_kalman_initialize(
    wk_kalman_state_t *state,
    const double *absolute_prediction,
    const double *reference,
    size_t calibration_count,
    double process_variance,
    double measurement_variance
);

/** Advance one post-calibration observation. */
double wk_kalman_update(
    wk_kalman_state_t *state,
    double absolute_prediction,
    double difference_prediction
);

#endif

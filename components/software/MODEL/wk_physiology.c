#include "wk_physiology.h"

#include <math.h>

static double round_two_decimals(double value)
{
    return round(value * 100.0) / 100.0;
}

esp_err_t wk_two_element_bp(
    double resistance,
    double compliance,
    double cardiac_cycle,
    double systolic_duration,
    double stroke_volume,
    unsigned int cycles,
    double *sbp,
    double *dbp
)
{
    const double pi = 3.14159265358979323846;
    double inflow;
    double tau;
    double denominator;
    double coefficient;
    double pressure_start = 80.0;
    double pressure_end = NAN;
    unsigned int cycle;
    if (sbp == NULL || dbp == NULL || resistance <= 0.0 || compliance <= 0.0
        || cardiac_cycle <= 0.0 || systolic_duration <= 0.0
        || cardiac_cycle <= systolic_duration || stroke_volume <= 0.0
        || cycles == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    inflow = stroke_volume * pi / (2.0 * systolic_duration);
    tau = resistance * compliance;
    denominator = systolic_duration * systolic_duration
        + (compliance * pi * resistance) * (compliance * pi * resistance);
    coefficient = inflow * systolic_duration * resistance
        * (compliance * pi * resistance) / denominator;
    for (cycle = 0u; cycle < cycles; ++cycle) {
        const double systolic_decay = exp(-systolic_duration / tau);
        const double diastolic_decay = exp(
            -(cardiac_cycle - systolic_duration) / tau
        );
        pressure_end = pressure_start * systolic_decay
            + coefficient * (systolic_decay + 1.0);
        pressure_start = pressure_end * diastolic_decay;
    }
    if (!isfinite(pressure_end) || !isfinite(pressure_start)) {
        return ESP_ERR_INVALID_STATE;
    }
    *sbp = round_two_decimals(pressure_end);
    *dbp = round_two_decimals(pressure_start);
    return ESP_OK;
}

esp_err_t wk_kalman_initialize(
    wk_kalman_state_t *state,
    const double *absolute_prediction,
    const double *reference,
    size_t calibration_count,
    double process_variance,
    double measurement_variance
)
{
    size_t index;
    double residual_mean = 0.0;
    double residual_variance = 0.0;
    double estimate;
    double variance;
    if (state == NULL || absolute_prediction == NULL || reference == NULL
        || calibration_count == 0u || process_variance < 0.0
        || measurement_variance <= 0.0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (index = 0u; index < calibration_count; ++index) {
        if (!isfinite(absolute_prediction[index]) || !isfinite(reference[index])) {
            return ESP_ERR_INVALID_ARG;
        }
        residual_mean += reference[index] - absolute_prediction[index];
    }
    residual_mean /= calibration_count;
    if (calibration_count > 1u) {
        for (index = 0u; index < calibration_count; ++index) {
            const double centered =
                (reference[index] - absolute_prediction[index]) - residual_mean;
            residual_variance += centered * centered;
        }
        /* NumPy var() in the PC pipeline uses ddof=0. */
        residual_variance /= calibration_count;
    }
    estimate = reference[0];
    variance = residual_variance + measurement_variance;
    for (index = 1u; index < calibration_count; ++index) {
        const double transition = reference[index] - reference[index - 1u];
        const double predicted = estimate + transition;
        const double predicted_variance = variance + process_variance;
        const double gain = predicted_variance / (predicted_variance + 1e-6);
        estimate = predicted + gain * (reference[index] - predicted);
        variance = (1.0 - gain) * predicted_variance;
    }
    state->initialized = true;
    state->estimate = estimate;
    state->variance = variance;
    state->process_variance = process_variance;
    state->measurement_variance = measurement_variance;
    return ESP_OK;
}

double wk_kalman_update(
    wk_kalman_state_t *state,
    double absolute_prediction,
    double difference_prediction
)
{
    double predicted;
    double predicted_variance;
    double gain;
    if (state == NULL || !state->initialized || !isfinite(absolute_prediction)) {
        return NAN;
    }
    if (!isfinite(difference_prediction)) {
        difference_prediction = 0.0;
    }
    predicted = state->estimate + difference_prediction;
    predicted_variance = state->variance + state->process_variance;
    gain = predicted_variance
        / (predicted_variance + state->measurement_variance);
    state->estimate = predicted + gain * (absolute_prediction - predicted);
    state->variance = (1.0 - gain) * predicted_variance;
    return state->estimate;
}

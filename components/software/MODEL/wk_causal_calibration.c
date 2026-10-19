#include "wk_causal_calibration.h"

#include <math.h>
#include <string.h>

#include "wk_physiology.h"

#define WK_EPS 1e-12

static double clampd(double value, double lower, double upper)
{
    if (value < lower) return lower;
    if (value > upper) return upper;
    return value;
}

static double trailing_mean(const wk_causal_daaf_t *state)
{
    size_t index;
    double sum = 0.0;
    for (index = 0u; index < state->static_count; ++index) {
        sum += state->static_history[index];
    }
    return state->static_count > 0u ? sum / state->static_count : 0.0;
}

static double causal_level_recenter(
    wk_causal_daaf_t *state,
    double absolute,
    double calibrated
)
{
    size_t index;
    double absolute_mean = 0.0;
    double calibrated_mean = 0.0;
    const size_t slot = state->level_write;
    state->absolute_level[slot] = absolute;
    state->calibrated_level[slot] = calibrated;
    state->level_write = (slot + 1u) % WK_DAAF_HISTORY;
    if (state->level_count < WK_DAAF_HISTORY) ++state->level_count;
    for (index = 0u; index < state->level_count; ++index) {
        absolute_mean += state->absolute_level[index];
        calibrated_mean += state->calibrated_level[index];
    }
    absolute_mean /= state->level_count;
    calibrated_mean /= state->level_count;
    calibrated += absolute_mean - calibrated_mean;
    state->calibrated_level[slot] = calibrated;
    return calibrated;
}

static void rolling_statistics(
    const wk_causal_daaf_t *state,
    double *correlation,
    double *slope
)
{
    size_t index;
    double mean_x = 0.0;
    double mean_y = 0.0;
    double covariance = 0.0;
    double variance_x = 0.0;
    double variance_y = 0.0;
    if (state->difference_count < 2u) {
        *correlation = 0.0;
        *slope = 0.0;
        return;
    }
    for (index = 0u; index < state->difference_count; ++index) {
        mean_x += state->driver_difference[index];
        mean_y += state->prediction_difference[index];
    }
    mean_x /= state->difference_count;
    mean_y /= state->difference_count;
    for (index = 0u; index < state->difference_count; ++index) {
        const double x = state->driver_difference[index] - mean_x;
        const double y = state->prediction_difference[index] - mean_y;
        covariance += x * y;
        variance_x += x * x;
        variance_y += y * y;
    }
    *slope = variance_x > WK_EPS ? covariance / variance_x : 0.0;
    *correlation = variance_x > WK_EPS && variance_y > WK_EPS
        ? covariance / sqrt(variance_x * variance_y) : 0.0;
}

static double causal_daaf_update(
    wk_causal_daaf_t *state,
    double driver,
    double absolute_prediction
)
{
    const double beta = 0.95;
    const double target_correlation = 0.90;
    double result;
    state->static_history[state->static_write] = absolute_prediction;
    state->static_write = (state->static_write + 1u) % 3u;
    if (state->static_count < 3u) ++state->static_count;
    if (!state->has_previous) {
        state->has_previous = true;
        state->previous_driver = driver;
        state->previous_absolute = absolute_prediction;
        state->previous_daaf = causal_level_recenter(
            state, absolute_prediction, absolute_prediction
        );
        return absolute_prediction;
    }
    {
        const double driver_difference = driver - state->previous_driver;
        const double prediction_difference =
            absolute_prediction - state->previous_absolute;
        double correlation;
        double slope;
        double alpha;
        double scale;
        double anchored_difference;
        double calibrated_difference;
        const size_t slot = state->difference_write;
        state->driver_difference[slot] = driver_difference;
        state->prediction_difference[slot] = prediction_difference;
        state->difference_write = (slot + 1u) % WK_DAAF_HISTORY;
        if (state->difference_count < WK_DAAF_HISTORY) ++state->difference_count;
        rolling_statistics(state, &correlation, &slope);
        alpha = clampd(correlation / target_correlation, 0.0, 1.0);
        scale = fabs(correlation) > 0.05
            ? target_correlation / fabs(correlation) : 1.0;
        anchored_difference = scale * slope * driver_difference;
        calibrated_difference = alpha * anchored_difference
            + (1.0 - alpha) * prediction_difference;
        result = beta * (state->previous_daaf + calibrated_difference)
            + (1.0 - beta) * trailing_mean(state);
    }
    state->previous_driver = driver;
    state->previous_absolute = absolute_prediction;
    result = causal_level_recenter(state, absolute_prediction, result);
    state->previous_daaf = fmax(result, 1e-6);
    return state->previous_daaf;
}

void wk_online_parameter_init(
    wk_online_parameter_t *state,
    double process_variance,
    double measurement_variance
)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->process_variance = process_variance;
    state->measurement_variance = measurement_variance;
}

double wk_online_parameter_update(
    wk_online_parameter_t *state,
    double absolute_prediction,
    double difference_prediction,
    double driver_sv,
    const double *reference
)
{
    double daaf;
    double bias = 1.0;
    double measurement;
    double transition;
    double predicted;
    double predicted_variance;
    double measurement_variance;
    double gain;
    const bool reference_valid = reference != NULL
        && isfinite(*reference) && *reference > 0.0;
    if (state == NULL || !isfinite(absolute_prediction)
        || !isfinite(driver_sv)) return NAN;
    daaf = causal_daaf_update(&state->daaf, driver_sv, absolute_prediction);
    if (reference_valid && state->bias_count < WK_REFERENCE_CALIBRATION_POINTS) {
        state->bias_ratio_sum += daaf / *reference;
        ++state->bias_count;
    }
    if (state->bias_count > 0u) {
        bias = state->bias_ratio_sum / state->bias_count;
        if (!isfinite(bias) || bias <= 0.0) bias = 1.0;
    }
    measurement = fmax(daaf / bias, 1e-6);
    if (!state->kalman_initialized) {
        state->kalman_initialized = true;
        state->estimate = reference_valid ? *reference : measurement;
        state->variance = state->measurement_variance;
        if (reference_valid) {
            state->has_previous_reference = true;
            state->previous_reference = *reference;
        }
        return state->estimate;
    }
    transition = isfinite(difference_prediction) ? difference_prediction : 0.0;
    if (reference_valid && state->has_previous_reference) {
        transition = *reference - state->previous_reference;
    }
    predicted = state->estimate + transition;
    predicted_variance = state->variance + state->process_variance;
    measurement_variance = reference_valid ? 1e-6 : state->measurement_variance;
    gain = predicted_variance / (predicted_variance + measurement_variance);
    state->estimate = predicted + gain
        * ((reference_valid ? *reference : measurement) - predicted);
    state->variance = (1.0 - gain) * predicted_variance;
    if (reference_valid) {
        state->has_previous_reference = true;
        state->previous_reference = *reference;
    }
    return fmax(state->estimate, 1e-6);
}

void wk_online_bp_init(
    wk_online_bp_t *state,
    double process_variance,
    double measurement_variance
)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->process_variance = process_variance;
    state->measurement_variance = measurement_variance;
}

double wk_online_bp_update(
    wk_online_bp_t *state,
    double absolute_prediction,
    double difference_prediction,
    const double *reference
)
{
    double transition;
    double predicted;
    double predicted_variance;
    double measurement;
    double calibrated_absolute;
    double measurement_variance;
    double gain;
    const bool reference_valid = reference != NULL && isfinite(*reference);
    if (state == NULL || !isfinite(absolute_prediction)) return NAN;
    if (reference_valid && state->reference_count < WK_REFERENCE_CALIBRATION_POINTS) {
        state->absolute_bias_sum += absolute_prediction - *reference;
    }
    calibrated_absolute = state->reference_count > 0u
        ? absolute_prediction - state->absolute_bias_sum / state->reference_count
        : absolute_prediction;
    if (!state->initialized) {
        state->initialized = true;
        state->estimate = reference_valid ? *reference : absolute_prediction;
        state->variance = state->measurement_variance;
        if (reference_valid) {
            state->has_previous_reference = true;
            state->previous_reference = *reference;
            state->reference_count = 1u;
        }
        return state->estimate;
    }
    transition = isfinite(difference_prediction) ? difference_prediction : 0.0;
    if (reference_valid && state->has_previous_reference
        && state->reference_count < WK_REFERENCE_CALIBRATION_POINTS) {
        transition = *reference - state->previous_reference;
    }
    predicted = state->estimate + transition;
    predicted_variance = state->variance + state->process_variance;
    measurement = reference_valid ? *reference : calibrated_absolute;
    measurement_variance = reference_valid
        && state->reference_count < WK_REFERENCE_CALIBRATION_POINTS
        ? 1e-6 : state->measurement_variance;
    gain = predicted_variance / (predicted_variance + measurement_variance);
    state->estimate = predicted + gain * (measurement - predicted);
    state->variance = (1.0 - gain) * predicted_variance;
    if (reference_valid && state->reference_count < WK_REFERENCE_CALIBRATION_POINTS) {
        state->has_previous_reference = true;
        state->previous_reference = *reference;
        ++state->reference_count;
    }
    return state->estimate;
}

static double wk_reference_loss(
    double resistance,
    double compliance,
    double reference_sbp,
    double reference_dbp,
    double cardiac_cycle,
    double systolic_duration,
    double stroke_volume
)
{
    double sbp;
    double dbp;
    if (wk_two_element_bp(resistance, compliance, cardiac_cycle,
                          systolic_duration, stroke_volume, 20u,
                          &sbp, &dbp) != ESP_OK) return HUGE_VAL;
    return (sbp - reference_sbp) * (sbp - reference_sbp)
        + (dbp - reference_dbp) * (dbp - reference_dbp);
}

esp_err_t wk_inverse_wk_reference(
    double reference_sbp,
    double reference_dbp,
    double cardiac_cycle,
    double systolic_duration,
    double stroke_volume,
    double *resistance,
    double *compliance
)
{
    double best_r = 1.0;
    double best_c = 0.8;
    double best_loss = HUGE_VAL;
    double step_r = 0.35;
    double step_c = 0.30;
    size_t row;
    size_t column;
    size_t iteration;
    if (resistance == NULL || compliance == NULL
        || !isfinite(reference_sbp) || !isfinite(reference_dbp)
        || reference_sbp <= reference_dbp || reference_dbp <= 0.0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A small coarse grid gives a deterministic, bounded initialization. */
    for (row = 0u; row < 12u; ++row) {
        const double r = 0.20 + row * (3.60 / 11.0);
        for (column = 0u; column < 12u; ++column) {
            const double c = 0.20 + column * (2.80 / 11.0);
            const double loss = wk_reference_loss(
                r, c, reference_sbp, reference_dbp,
                cardiac_cycle, systolic_duration, stroke_volume
            );
            if (loss < best_loss) {
                best_loss = loss;
                best_r = r;
                best_c = c;
            }
        }
    }
    /* Coordinate refinement avoids a heavy general-purpose optimizer. */
    for (iteration = 0u; iteration < 18u; ++iteration) {
        static const int directions[8][2] = {
            {-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
            {0, 1}, {1, -1}, {1, 0}, {1, 1}
        };
        bool improved = false;
        size_t direction;
        for (direction = 0u; direction < 8u; ++direction) {
            const double r = clampd(
                best_r + directions[direction][0] * step_r, 0.10, 5.0
            );
            const double c = clampd(
                best_c + directions[direction][1] * step_c, 0.10, 4.0
            );
            const double loss = wk_reference_loss(
                r, c, reference_sbp, reference_dbp,
                cardiac_cycle, systolic_duration, stroke_volume
            );
            if (loss < best_loss) {
                best_loss = loss;
                best_r = r;
                best_c = c;
                improved = true;
            }
        }
        if (!improved) {
            step_r *= 0.5;
            step_c *= 0.5;
        }
    }
    if (!isfinite(best_loss)) return ESP_ERR_INVALID_STATE;
    *resistance = best_r;
    *compliance = best_c;
    return ESP_OK;
}

#include "wk_realtime_pipeline.h"

#include <math.h>
#include <string.h>

#include "wk_physiology.h"

static bool valid_bp_pair(double sbp, double dbp)
{
    return isfinite(sbp) && isfinite(dbp)
        && sbp >= 50.0 && sbp <= 260.0
        && dbp >= 25.0 && dbp <= 160.0
        && sbp >= dbp + 5.0;
}

esp_err_t wk_realtime_init(
    wk_realtime_state_t *state,
    float finger_polarity,
    float wrist_polarity
)
{
    esp_err_t error;
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    wk_signal_stream_init(&state->stream, finger_polarity, wrist_polarity);
    wk_online_parameter_init(&state->resistance_filter, 0.1, 0.01);
    wk_online_parameter_init(&state->compliance_filter, 0.1, 0.01);
    /* Tuned on causal replay of the validation split, never the test split. */
    wk_online_bp_init(&state->sbp_filter, 0.0003, 0.3);
    wk_online_bp_init(&state->dbp_filter, 0.0003, 1.0);
    error = wk_edge_state_init(&state->edge);
    return error;
}

esp_err_t wk_realtime_set_reference(
    wk_realtime_state_t *state,
    double reference_sbp,
    double reference_dbp
)
{
    if (state == NULL || !valid_bp_pair(reference_sbp, reference_dbp)) {
        return ESP_ERR_INVALID_ARG;
    }
    state->reference_sbp = reference_sbp;
    state->reference_dbp = reference_dbp;
    state->reference_pending = true;
    return ESP_OK;
}

static esp_err_t process_latest_window(
    wk_realtime_state_t *state,
    wk_realtime_output_t *output
)
{
    wk_rc_model_output_t rc_models;
    wk_bp_model_output_t bp_models;
    double reference_r;
    double reference_c;
    double *reference_r_pointer = NULL;
    double *reference_c_pointer = NULL;
    double *reference_sbp_pointer = NULL;
    double *reference_dbp_pointer = NULL;
    double resistance;
    double compliance;
    double wk_sbp;
    double wk_dbp;
    double sbp;
    double dbp;
    double mechanism[6];
    const double cardiac_cycle = state->features[WK_F_TC];
    const double systolic_duration = state->features[WK_F_TS];
    const double stroke_volume = state->features[WK_F_SV];
    const double inverse_pat = state->features[WK_F_PWV];
    esp_err_t error;

    if (!wk_signal_stream_snapshot(
            &state->stream,
            state->ecg_window,
            state->finger_window,
            state->wrist_window)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wk_extract_features_250hz(
            state->ecg_window,
            state->finger_window,
            state->wrist_window,
            &state->feature_workspace,
            state->features,
            &output->signal_quality)) {
        output->status = WK_RT_SIGNAL_REJECTED;
        return ESP_OK;
    }
    error = wk_edge_predict_rc(&state->edge, state->features, &rc_models);
    if (error != ESP_OK) {
        output->status = WK_RT_MODEL_ERROR;
        return error;
    }
    if (state->reference_pending) {
        error = wk_inverse_wk_reference(
            state->reference_sbp,
            state->reference_dbp,
            cardiac_cycle,
            systolic_duration,
            stroke_volume,
            &reference_r,
            &reference_c
        );
        if (error == ESP_OK) {
            reference_r_pointer = &reference_r;
            reference_c_pointer = &reference_c;
            reference_sbp_pointer = &state->reference_sbp;
            reference_dbp_pointer = &state->reference_dbp;
            output->used_reference = true;
        }
        state->reference_pending = false;
    }
    resistance = wk_online_parameter_update(
        &state->resistance_filter,
        rc_models.r_absolute,
        rc_models.r_difference,
        stroke_volume,
        reference_r_pointer
    );
    compliance = wk_online_parameter_update(
        &state->compliance_filter,
        rc_models.c_absolute,
        rc_models.c_difference,
        stroke_volume,
        reference_c_pointer
    );
    error = wk_two_element_bp(
        resistance,
        compliance,
        cardiac_cycle,
        systolic_duration,
        stroke_volume,
        20u,
        &wk_sbp,
        &wk_dbp
    );
    if (error != ESP_OK) {
        output->status = WK_RT_PHYSIOLOGY_ERROR;
        return error;
    }
    mechanism[0] = wk_sbp;
    mechanism[1] = wk_dbp;
    mechanism[2] = resistance;
    mechanism[3] = compliance;
    mechanism[4] = stroke_volume;
    mechanism[5] = inverse_pat;
    error = wk_edge_predict_bp(
        &state->edge, state->features, mechanism, &bp_models
    );
    if (error != ESP_OK) {
        output->status = WK_RT_MODEL_ERROR;
        return error;
    }
    sbp = wk_online_bp_update(
        &state->sbp_filter,
        bp_models.sbp_absolute,
        bp_models.sbp_difference,
        reference_sbp_pointer
    );
    dbp = wk_online_bp_update(
        &state->dbp_filter,
        bp_models.dbp_absolute,
        bp_models.dbp_difference,
        reference_dbp_pointer
    );
    output->resistance = resistance;
    output->compliance = compliance;
    output->stroke_volume = stroke_volume;
    output->inverse_pat = inverse_pat;
    output->wk_sbp = wk_sbp;
    output->wk_dbp = wk_dbp;
    output->sbp = sbp;
    output->dbp = dbp;
    if (!valid_bp_pair(sbp, dbp)) {
        output->status = WK_RT_PHYSIOLOGY_ERROR;
        return ESP_ERR_INVALID_STATE;
    }
    output->status = WK_RT_OUTPUT_READY;
    return ESP_OK;
}

esp_err_t wk_realtime_push_sample(
    wk_realtime_state_t *state,
    float ecg,
    float finger_ppg,
    float wrist_ppg,
    wk_realtime_output_t *output
)
{
    if (state == NULL || output == NULL) return ESP_ERR_INVALID_ARG;
    memset(output, 0, sizeof(*output));
    ++state->sample_index;
    output->sample_index = state->sample_index;
    output->status = WK_RT_NO_NEW_WINDOW;
    if (!wk_signal_stream_push(&state->stream, ecg, finger_ppg, wrist_ppg)) {
        return ESP_OK;
    }
    return process_latest_window(state, output);
}

const char *wk_realtime_status_name(wk_realtime_status_t status)
{
    switch (status) {
        case WK_RT_NO_NEW_WINDOW: return "no_new_window";
        case WK_RT_OUTPUT_READY: return "output_ready";
        case WK_RT_SIGNAL_REJECTED: return "signal_rejected";
        case WK_RT_MODEL_ERROR: return "model_error";
        case WK_RT_PHYSIOLOGY_ERROR: return "physiology_error";
        default: return "unknown";
    }
}

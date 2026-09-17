#include "wk_edge_inference.h"

#include <math.h>
#include <string.h>

static esp_err_t find_models(wk_edge_state_t *state)
{
    state->abs_r_id = wk_flash_model_find(&state->models, "abs_R");
    state->abs_c_id = wk_flash_model_find(&state->models, "abs_C");
    state->diff_r_id = wk_flash_model_find(&state->models, "diff_R");
    state->diff_c_id = wk_flash_model_find(&state->models, "diff_C");
    state->abs_sbp_id = wk_flash_model_find(&state->models, "abs_SBP");
    state->abs_dbp_id = wk_flash_model_find(&state->models, "abs_DBP");
    state->diff_sbp_id = wk_flash_model_find(&state->models, "diff_SP");
    state->diff_dbp_id = wk_flash_model_find(&state->models, "diff_DP");
    return state->abs_r_id < 0 || state->abs_c_id < 0 || state->diff_r_id < 0 ||
        state->diff_c_id < 0 || state->abs_sbp_id < 0 || state->abs_dbp_id < 0 ||
        state->diff_sbp_id < 0 || state->diff_dbp_id < 0 ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t wk_edge_state_init(wk_edge_state_t *state)
{
    esp_err_t err;
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    err = wk_flash_models_init(&state->models);
    return err == ESP_OK ? find_models(state) : err;
}

esp_err_t wk_edge_predict_rc(wk_edge_state_t *state,
    const double features[WK_COMPACT_BASE_FEATURE_COUNT], wk_rc_model_output_t *output)
{
    esp_err_t err;
    if (state == NULL || features == NULL || output == NULL) return ESP_ERR_INVALID_ARG;
    err = wk_flash_predict_f64(&state->models, state->abs_r_id, features,
        WK_COMPACT_BASE_FEATURE_COUNT, &output->r_absolute);
    if (err != ESP_OK) return err;
    err = wk_flash_predict_f64(&state->models, state->abs_c_id, features,
        WK_COMPACT_BASE_FEATURE_COUNT, &output->c_absolute);
    if (err != ESP_OK) return err;
    state->difference_ready = state->has_previous;
    if (!state->difference_ready) { output->r_difference = NAN; output->c_difference = NAN; return ESP_OK; }
    wk_build_difference_input_f64(state->previous, features, WK_COMPACT_BASE_FEATURE_COUNT,
        state->difference_input);
    err = wk_flash_predict_f64(&state->models, state->diff_r_id, state->difference_input,
        4u * WK_COMPACT_BASE_FEATURE_COUNT, &output->r_difference);
    return err == ESP_OK ? wk_flash_predict_f64(&state->models, state->diff_c_id,
        state->difference_input, 4u * WK_COMPACT_BASE_FEATURE_COUNT, &output->c_difference) : err;
}

esp_err_t wk_edge_predict_bp(wk_edge_state_t *state,
    const double features[WK_COMPACT_BASE_FEATURE_COUNT], const double mechanism[6],
    wk_bp_model_output_t *output)
{
    esp_err_t err;
    if (state == NULL || features == NULL || mechanism == NULL || output == NULL)
        return ESP_ERR_INVALID_ARG;
    err = wk_flash_predict_f64(&state->models, state->abs_sbp_id, mechanism, 6u,
                               &output->sbp_absolute);
    if (err == ESP_OK) err = wk_flash_predict_f64(&state->models, state->abs_dbp_id,
        mechanism, 6u, &output->dbp_absolute);
    if (err == ESP_OK && state->difference_ready) {
        err = wk_flash_predict_f64(&state->models, state->diff_sbp_id,
            state->difference_input, 4u * WK_COMPACT_BASE_FEATURE_COUNT, &output->sbp_difference);
        if (err == ESP_OK) err = wk_flash_predict_f64(&state->models, state->diff_dbp_id,
            state->difference_input, 4u * WK_COMPACT_BASE_FEATURE_COUNT, &output->dbp_difference);
    } else if (err == ESP_OK) { output->sbp_difference = NAN; output->dbp_difference = NAN; }
    if (err == ESP_OK) { memcpy(state->previous, features, sizeof(state->previous)); state->has_previous = true; state->difference_ready = false; }
    return err;
}

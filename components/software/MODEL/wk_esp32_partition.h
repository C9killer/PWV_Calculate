#ifndef WK_ESP32_PARTITION_H
#define WK_ESP32_PARTITION_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The deployed package contains eight compact LightGBM tree ensembles. */
#define WK_FLASH_MAX_MODELS 8u
#define WK_FLASH_DESCRIPTOR_BYTES 44u
/* Existing partitions.csv declares xgbmodel as `data, 0x40`. */
#define WK_MODEL_PARTITION_TYPE ESP_PARTITION_TYPE_DATA
#define WK_MODEL_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x40)
#define WK_MODEL_PARTITION_LABEL "xgbmodel"
#define WK_COMPACT_BASE_FEATURE_COUNT 185u

typedef struct {
    const esp_partition_t *partition;
    uint32_t model_count;
    uint32_t base_feature_count;
    uint8_t descriptors[WK_FLASH_MAX_MODELS][WK_FLASH_DESCRIPTOR_BYTES];
} wk_flash_models_t;

/* Reads only the package header and descriptors; no model payload enters RAM. */
esp_err_t wk_flash_models_init(wk_flash_models_t *context);
int wk_flash_model_find(const wk_flash_models_t *context, const char *name);
size_t wk_flash_model_input_count(const wk_flash_models_t *context,
                                  uint32_t model_id);

/* Maps just the selected model payload, evaluates it, and immediately unmaps. */
esp_err_t wk_flash_predict_f64(const wk_flash_models_t *context,
                               uint32_t model_id, const double *features,
                               size_t feature_count, double *prediction);

void wk_build_difference_input_f64(const double *previous,
                                   const double *current,
                                   size_t feature_count,
                                   double *difference_input);

#ifdef __cplusplus
}
#endif

#endif

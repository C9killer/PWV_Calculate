#include "ModelStorage.h"

#include <inttypes.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "MODEL_STORAGE";
static const esp_partition_t *model_partition;

esp_err_t ModelStorage_Init(void)
{
    if (model_partition != NULL) {
        return ESP_OK;
    }
    model_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                               ESP_PARTITION_SUBTYPE_ANY,
                                               MODEL_STORAGE_PARTITION_LABEL);
    if (model_partition == NULL) {
        ESP_LOGE(TAG, "Partition '%s' not found",
                 MODEL_STORAGE_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Partition '%s': address=0x%08" PRIx32 ", size=%u bytes",
             model_partition->label, model_partition->address,
             (unsigned)model_partition->size);
    return ESP_OK;
}

size_t ModelStorage_GetSize(void)
{
    return model_partition != NULL ? model_partition->size : 0;
}

esp_err_t ModelStorage_ReadChunk(uint32_t offset, void *buffer,
                                size_t buffer_capacity, size_t *out_read)
{
    if (buffer == NULL || buffer_capacity == 0 || out_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_read = 0;
    if (model_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset >= model_partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t remaining = model_partition->size - offset;
    const size_t length = buffer_capacity < remaining
                              ? buffer_capacity
                              : remaining;
    esp_err_t err = esp_partition_read(model_partition, offset, buffer, length);
    if (err == ESP_OK) {
        *out_read = length;
    }
    return err;
}

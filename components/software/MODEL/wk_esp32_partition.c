#include "wk_esp32_partition.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

#define WK_MAGIC "WKESP1\0\0"
#define WK_VERSION 1u
#define WK_VALUE_F64 2u
#define WK_LEAF_FLAG 0x8000u
#define WK_REF_INDEX 0x7fffu

typedef struct { uint8_t magic[8]; uint32_t version, model_count, base_feature_count, descriptor_offset, total_bytes, reserved; } wk_header_disk_t;
typedef struct { char name[16]; uint8_t input_kind, value_type; uint16_t reserved; uint32_t tree_count, node_count, leaf_count, tree_offset, node_offset, leaf_offset; } wk_model_disk_t;
typedef struct { uint32_t node_start, leaf_start; uint16_t root_ref, node_count, leaf_count, reserved; } wk_tree_disk_t;
typedef struct { double threshold; uint16_t feature_source, left_ref, right_ref, flags; } wk_node64_disk_t;

_Static_assert(sizeof(wk_header_disk_t) == 32, "WKESP1 header mismatch");
_Static_assert(sizeof(wk_model_disk_t) == WK_FLASH_DESCRIPTOR_BYTES, "WKESP1 descriptor mismatch");
_Static_assert(sizeof(wk_tree_disk_t) == 16, "WKESP1 tree mismatch");
_Static_assert(sizeof(wk_node64_disk_t) == 16, "WKESP1 node mismatch");

static const char *TAG = "WK_MODEL";

static void copy_model(const wk_flash_models_t *context, uint32_t id,
                       wk_model_disk_t *model)
{
    memcpy(model, context->descriptors[id], sizeof(*model));
}

esp_err_t wk_flash_models_init(wk_flash_models_t *context)
{
    wk_header_disk_t header;
    size_t descriptor_bytes;
    uint32_t id;
    esp_err_t error;
    if (context == NULL) return ESP_ERR_INVALID_ARG;
    memset(context, 0, sizeof(*context));
    context->partition = esp_partition_find_first(WK_MODEL_PARTITION_TYPE,
        WK_MODEL_PARTITION_SUBTYPE, WK_MODEL_PARTITION_LABEL);
    if (context->partition == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", WK_MODEL_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    error = esp_partition_read(context->partition, 0, &header, sizeof(header));
    if (error != ESP_OK) return error;
    if (memcmp(header.magic, WK_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != WK_VERSION || header.model_count == 0 ||
        header.model_count > WK_FLASH_MAX_MODELS ||
        header.total_bytes > context->partition->size) {
        ESP_LOGE(TAG, "invalid or unflashed WKESP1 package");
        return ESP_ERR_INVALID_RESPONSE;
    }
    descriptor_bytes = (size_t)header.model_count * sizeof(wk_model_disk_t);
    if ((size_t)header.descriptor_offset + descriptor_bytes > header.total_bytes)
        return ESP_ERR_INVALID_SIZE;
    error = esp_partition_read(context->partition, header.descriptor_offset,
                               context->descriptors, descriptor_bytes);
    if (error != ESP_OK) return error;
    context->model_count = header.model_count;
    context->base_feature_count = header.base_feature_count;
    context->package_bytes = header.total_bytes;
    if (context->base_feature_count != WK_COMPACT_BASE_FEATURE_COUNT)
        return ESP_ERR_INVALID_RESPONSE;
    for (id = 0; id < context->model_count; ++id) {
        wk_model_disk_t model;
        copy_model(context, id, &model);
        if (model.value_type != WK_VALUE_F64 || model.input_kind > 2u)
            return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "WKESP1 ready: %lu models, %lu bytes in flash partition",
             (unsigned long)context->model_count, (unsigned long)header.total_bytes);
    return ESP_OK;
}

int wk_flash_model_find(const wk_flash_models_t *context, const char *name)
{
    uint32_t id;
    if (context == NULL || name == NULL) return -1;
    for (id = 0; id < context->model_count; ++id) {
        wk_model_disk_t model;
        copy_model(context, id, &model);
        if (strncmp(model.name, name, sizeof(model.name)) == 0) return (int)id;
    }
    return -1;
}

size_t wk_flash_model_input_count(const wk_flash_models_t *context, uint32_t id)
{
    wk_model_disk_t model;
    if (context == NULL || id >= context->model_count) return 0;
    copy_model(context, id, &model);
    return model.input_kind == 0u ? context->base_feature_count :
           model.input_kind == 1u ? 4u * (size_t)context->base_feature_count : 6u;
}

esp_err_t wk_flash_predict_f64(const wk_flash_models_t *context, uint32_t id,
                               const double *features, size_t feature_count,
                               double *prediction)
{
    wk_model_disk_t model;
    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle = 0;
    const wk_tree_disk_t *trees;
    const wk_node64_disk_t *nodes;
    const double *leaves;
    size_t map_start, node_end, leaf_end, map_end;
    uint32_t tree_id;
    double sum = 0.0;
    esp_err_t error;
    if (context == NULL || features == NULL || prediction == NULL || id >= context->model_count)
        return ESP_ERR_INVALID_ARG;
    if (feature_count != wk_flash_model_input_count(context, id)) return ESP_ERR_INVALID_SIZE;
    copy_model(context, id, &model);
    map_start = model.tree_offset;
    node_end = (size_t)model.node_offset + (size_t)model.node_count * sizeof(*nodes);
    leaf_end = (size_t)model.leaf_offset + (size_t)model.leaf_count * sizeof(*leaves);
    map_end = node_end > leaf_end ? node_end : leaf_end;
    if (map_start >= map_end || map_end > context->partition->size) return ESP_ERR_INVALID_SIZE;
    error = esp_partition_mmap(context->partition, map_start, map_end - map_start,
        ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (error != ESP_OK) return error;
    trees = (const wk_tree_disk_t *)((const uint8_t *)mapped + model.tree_offset - map_start);
    nodes = (const wk_node64_disk_t *)((const uint8_t *)mapped + model.node_offset - map_start);
    leaves = (const double *)((const uint8_t *)mapped + model.leaf_offset - map_start);
    for (tree_id = 0; tree_id < model.tree_count; ++tree_id) {
        const wk_tree_disk_t *tree = &trees[tree_id];
        uint16_t reference = tree->root_ref;
        while ((reference & WK_LEAF_FLAG) == 0u) {
            uint16_t local = reference & WK_REF_INDEX;
            const wk_node64_disk_t *node;
            double value;
            if (local >= tree->node_count) { error = ESP_ERR_INVALID_RESPONSE; goto done; }
            node = &nodes[tree->node_start + local];
            if (node->feature_source >= feature_count) { error = ESP_ERR_INVALID_SIZE; goto done; }
            value = features[node->feature_source];
            reference = isnan(value) ? ((node->flags & 1u) ? node->left_ref : node->right_ref) :
                (value <= node->threshold ? node->left_ref : node->right_ref);
        }
        if ((reference & WK_REF_INDEX) >= tree->leaf_count) { error = ESP_ERR_INVALID_RESPONSE; goto done; }
        sum += leaves[tree->leaf_start + (reference & WK_REF_INDEX)];
    }
    *prediction = sum;
    error = ESP_OK;
done:
    esp_partition_munmap(handle);
    return error;
}

void wk_build_difference_input_f64(const double *previous, const double *current,
                                   size_t count, double *output)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        const double delta = current[i] - previous[i];
        output[i] = previous[i]; output[count + i] = current[i];
        output[2u * count + i] = delta; output[3u * count + i] = fabs(delta);
    }
}

#ifndef MODEL_STORAGE_H
#define MODEL_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MODEL_STORAGE_PARTITION_LABEL "xgbmodel"
#define MODEL_STORAGE_DEFAULT_CHUNK_SIZE (16U * 1024U)

/* Locate the 12 MiB model partition in the ESP32-S3 module Flash. */
esp_err_t ModelStorage_Init(void);
size_t ModelStorage_GetSize(void);

/* Read at most buffer_capacity bytes. out_read reports the actual byte count
 * at the end of the partition. Reuse one PSRAM buffer between calls instead
 * of allocating/freeing a buffer for every model node or tree.
 */
esp_err_t ModelStorage_ReadChunk(uint32_t offset, void *buffer,
                                size_t buffer_capacity, size_t *out_read);

#endif

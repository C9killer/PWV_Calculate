#ifndef W25Q64_H
#define W25Q64_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* W25Q64JV is connected to the ESP32-S3 general-purpose SPI3 host.
 * SPI2 is reserved by ADS131M04. GPIO48 is also assigned to the current
 * WS2812 driver, so LED_Init() and this driver must not be used together.
 */
#define W25Q64_MISO_PIN       42
#define W25Q64_MOSI_PIN       41
#define W25Q64_SCK_PIN        40
#define W25Q64_CS_PIN         48
#define W25Q64_SPI_CLOCK_HZ   20000000

#define W25Q64_CAPACITY_BYTES (8U * 1024U * 1024U)
#define W25Q64_PAGE_SIZE      256U
#define W25Q64_SECTOR_SIZE    4096U

typedef struct {
    uint8_t manufacturer;
    uint8_t memory_type;
    uint8_t capacity;
} W25Q64_JedecId;

/* Initialize SPI3 and verify the expected Winbond EF 40 17 JEDEC ID. */
esp_err_t W25Q64_Init(void);
esp_err_t W25Q64_Deinit(void);

esp_err_t W25Q64_ReadJedecId(W25Q64_JedecId *id);
esp_err_t W25Q64_ReadStatus(uint8_t *status);
esp_err_t W25Q64_Read(uint32_t address, void *data, size_t length);

/* Programming only changes erased bits from 1 to 0. W25Q64_Write() splits
 * the operation at 256-byte page boundaries and waits after every page.
 */
esp_err_t W25Q64_Write(uint32_t address, const void *data, size_t length);

/* The sector address must be 4 KiB aligned. */
esp_err_t W25Q64_EraseSector(uint32_t address);
esp_err_t W25Q64_EraseChip(void);

#endif

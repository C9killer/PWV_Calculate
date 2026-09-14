#include "W25Q64.h"

#include <stdbool.h>
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define W25Q64_CMD_WRITE_ENABLE  0x06
#define W25Q64_CMD_READ_STATUS1  0x05
#define W25Q64_CMD_READ_DATA     0x03
#define W25Q64_CMD_PAGE_PROGRAM  0x02
#define W25Q64_CMD_SECTOR_ERASE  0x20
#define W25Q64_CMD_CHIP_ERASE    0xC7
#define W25Q64_CMD_JEDEC_ID      0x9F
#define W25Q64_CMD_RELEASE_PD    0xAB
#define W25Q64_CMD_RESET_ENABLE  0x66
#define W25Q64_CMD_RESET         0x99

#define W25Q64_STATUS_BUSY       (1U << 0)
#define W25Q64_STATUS_WEL        (1U << 1)
#define W25Q64_TRANSFER_CHUNK    4096U

#define W25Q64_PAGE_TIMEOUT_MS   20U
#define W25Q64_SECTOR_TIMEOUT_MS 2000U
#define W25Q64_CHIP_TIMEOUT_MS   120000U

static const char *TAG = "W25Q64";
static spi_device_handle_t flash_device;
static SemaphoreHandle_t flash_mutex;
static bool spi_bus_owned;

static bool range_valid(uint32_t address, size_t length)
{
    return address <= W25Q64_CAPACITY_BYTES &&
           length <= (size_t)(W25Q64_CAPACITY_BYTES - address);
}

static esp_err_t transmit(uint8_t command, uint32_t address,
                          uint8_t address_bits, const void *tx_data,
                          void *rx_data, size_t length)
{
    spi_transaction_ext_t transaction = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_ADDR,
            .cmd = command,
            .addr = address,
            .length = tx_data != NULL ? length * 8U : 0,
            .rxlength = rx_data != NULL ? length * 8U : 0,
            .tx_buffer = tx_data,
            .rx_buffer = rx_data,
        },
        .address_bits = address_bits,
    };
    return spi_device_transmit(flash_device, &transaction.base);
}

static esp_err_t command_only(uint8_t command)
{
    return transmit(command, 0, 0, NULL, NULL, 0);
}

static esp_err_t read_status_unlocked(uint8_t *status)
{
    return transmit(W25Q64_CMD_READ_STATUS1, 0, 0, NULL, status, 1);
}

static esp_err_t wait_ready_unlocked(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms) + 1;
    for (;;) {
        uint8_t status;
        esp_err_t err = read_status_unlocked(&status);
        if (err != ESP_OK) {
            return err;
        }
        if ((status & W25Q64_STATUS_BUSY) == 0) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1) + 1);
    }
}

static esp_err_t write_enable_unlocked(void)
{
    esp_err_t err = command_only(W25Q64_CMD_WRITE_ENABLE);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t status;
    err = read_status_unlocked(&status);
    if (err != ESP_OK) {
        return err;
    }
    return (status & W25Q64_STATUS_WEL) != 0
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

static esp_err_t lock_driver(void)
{
    if (flash_device == NULL || flash_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(flash_mutex, portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void unlock_driver(void)
{
    xSemaphoreGive(flash_mutex);
}

esp_err_t W25Q64_Init(void)
{
    if (flash_device != NULL) {
        return ESP_OK;
    }

    flash_mutex = xSemaphoreCreateMutex();
    if (flash_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const spi_bus_config_t bus = {
        .mosi_io_num = W25Q64_MOSI_PIN,
        .miso_io_num = W25Q64_MISO_PIN,
        .sclk_io_num = W25Q64_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = W25Q64_TRANSFER_CHUNK,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        goto fail;
    }
    spi_bus_owned = true;

    const spi_device_interface_config_t device = {
        .command_bits = 8,
        .address_bits = 0,
        .mode = 0,
        .clock_speed_hz = W25Q64_SPI_CLOCK_HZ,
        .spics_io_num = W25Q64_CS_PIN,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(SPI3_HOST, &device, &flash_device);
    if (err != ESP_OK) {
        goto fail;
    }

    /* Recover from deep power-down or an interrupted command sequence. */
    err = command_only(W25Q64_CMD_RELEASE_PD);
    if (err != ESP_OK) {
        goto fail;
    }
    esp_rom_delay_us(5);
    err = command_only(W25Q64_CMD_RESET_ENABLE);
    if (err != ESP_OK) {
        goto fail;
    }
    err = command_only(W25Q64_CMD_RESET);
    if (err != ESP_OK) {
        goto fail;
    }
    esp_rom_delay_us(50);

    W25Q64_JedecId id;
    err = W25Q64_ReadJedecId(&id);
    if (err != ESP_OK) {
        goto fail;
    }
    if (id.manufacturer != 0xEF || id.memory_type != 0x40 ||
        id.capacity != 0x17) {
        ESP_LOGE(TAG, "Unexpected JEDEC ID: %02X %02X %02X (expected EF 40 17)",
                 id.manufacturer, id.memory_type, id.capacity);
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    ESP_LOGI(TAG,
             "SPI3 ready: JEDEC=%02X%02X%02X, 8 MiB, CS=%d MOSI=%d SCK=%d MISO=%d",
             id.manufacturer, id.memory_type, id.capacity, W25Q64_CS_PIN,
             W25Q64_MOSI_PIN, W25Q64_SCK_PIN, W25Q64_MISO_PIN);
    return ESP_OK;

fail:
    if (flash_device != NULL) {
        spi_bus_remove_device(flash_device);
        flash_device = NULL;
    }
    if (spi_bus_owned) {
        spi_bus_free(SPI3_HOST);
        spi_bus_owned = false;
    }
    if (flash_mutex != NULL) {
        vSemaphoreDelete(flash_mutex);
        flash_mutex = NULL;
    }
    return err;
}

esp_err_t W25Q64_Deinit(void)
{
    if (flash_device == NULL) {
        return ESP_OK;
    }
    esp_err_t err = lock_driver();
    if (err != ESP_OK) {
        return err;
    }
    spi_device_handle_t device = flash_device;
    flash_device = NULL;
    unlock_driver();

    err = spi_bus_remove_device(device);
    if (err == ESP_OK && spi_bus_owned) {
        err = spi_bus_free(SPI3_HOST);
        if (err == ESP_OK) {
            spi_bus_owned = false;
        }
    }
    if (err == ESP_OK) {
        vSemaphoreDelete(flash_mutex);
        flash_mutex = NULL;
    }
    return err;
}

esp_err_t W25Q64_ReadJedecId(W25Q64_JedecId *id)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_driver();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t raw[3];
    err = transmit(W25Q64_CMD_JEDEC_ID, 0, 0, NULL, raw, sizeof(raw));
    if (err == ESP_OK) {
        id->manufacturer = raw[0];
        id->memory_type = raw[1];
        id->capacity = raw[2];
    }
    unlock_driver();
    return err;
}

esp_err_t W25Q64_ReadStatus(uint8_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_driver();
    if (err == ESP_OK) {
        err = read_status_unlocked(status);
        unlock_driver();
    }
    return err;
}

esp_err_t W25Q64_Read(uint32_t address, void *data, size_t length)
{
    if ((data == NULL && length != 0) || !range_valid(address, length)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 0) {
        return ESP_OK;
    }
    esp_err_t err = lock_driver();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *output = data;
    while (length != 0) {
        const size_t chunk = length > W25Q64_TRANSFER_CHUNK
                                 ? W25Q64_TRANSFER_CHUNK
                                 : length;
        err = transmit(W25Q64_CMD_READ_DATA, address, 24, NULL, output, chunk);
        if (err != ESP_OK) {
            break;
        }
        address += (uint32_t)chunk;
        output += chunk;
        length -= chunk;
    }
    unlock_driver();
    return err;
}

esp_err_t W25Q64_Write(uint32_t address, const void *data, size_t length)
{
    if ((data == NULL && length != 0) || !range_valid(address, length)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 0) {
        return ESP_OK;
    }
    esp_err_t err = lock_driver();
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t *input = data;
    while (length != 0) {
        const size_t page_remaining = W25Q64_PAGE_SIZE -
                                      (address % W25Q64_PAGE_SIZE);
        const size_t chunk = length < page_remaining ? length : page_remaining;
        err = write_enable_unlocked();
        if (err != ESP_OK) {
            break;
        }
        err = transmit(W25Q64_CMD_PAGE_PROGRAM, address, 24, input, NULL, chunk);
        if (err != ESP_OK) {
            break;
        }
        err = wait_ready_unlocked(W25Q64_PAGE_TIMEOUT_MS);
        if (err != ESP_OK) {
            break;
        }
        address += (uint32_t)chunk;
        input += chunk;
        length -= chunk;
    }
    unlock_driver();
    return err;
}

esp_err_t W25Q64_EraseSector(uint32_t address)
{
    if (address >= W25Q64_CAPACITY_BYTES ||
        (address % W25Q64_SECTOR_SIZE) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_driver();
    if (err != ESP_OK) {
        return err;
    }
    err = write_enable_unlocked();
    if (err == ESP_OK) {
        err = transmit(W25Q64_CMD_SECTOR_ERASE, address, 24, NULL, NULL, 0);
    }
    if (err == ESP_OK) {
        err = wait_ready_unlocked(W25Q64_SECTOR_TIMEOUT_MS);
    }
    unlock_driver();
    return err;
}

esp_err_t W25Q64_EraseChip(void)
{
    esp_err_t err = lock_driver();
    if (err != ESP_OK) {
        return err;
    }
    err = write_enable_unlocked();
    if (err == ESP_OK) {
        err = command_only(W25Q64_CMD_CHIP_ERASE);
    }
    if (err == ESP_OK) {
        err = wait_ready_unlocked(W25Q64_CHIP_TIMEOUT_MS);
    }
    unlock_driver();
    return err;
}

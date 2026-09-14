#include "ADS131.h"

#include <stddef.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ADS131_FRAME_BYTES 18 /* Response + four channels + CRC, each 24 bits. */
#define ADS131_CMD_RREG    0xA000u

static const char *TAG = "ADS131";
static spi_device_handle_t adc;
static portMUX_TYPE sync_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t exchange(const uint8_t *tx, uint8_t *rx)
{
    spi_transaction_t transaction = {
        .length = ADS131_FRAME_BYTES * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(adc, &transaction);
}

static esp_err_t transfer_frame(uint16_t command, uint16_t *response)
{
    /* Commands and register responses are 16-bit, MSB aligned in 24 bits.
     * One transaction keeps CS low over all six words (144 SCLK cycles).
     */
    uint8_t tx[ADS131_FRAME_BYTES] = {command >> 8, command & 0xFF, 0};
    uint8_t rx[ADS131_FRAME_BYTES] = {0};
    esp_err_t err = exchange(tx, rx);
    if (err == ESP_OK && response != NULL) {
        *response = ((uint16_t)rx[0] << 8) | rx[1];
    }
    return err;
}

esp_err_t ADS131_Init(void)
{
    if (adc != NULL) {
        return ESP_OK;
    }
    const spi_bus_config_t bus = {
        .mosi_io_num = ADS131_MOSI_PIN,
        .miso_io_num = ADS131_MISO_PIN,
        .sclk_io_num = ADS131_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ADS131_FRAME_BYTES,
    };
    /* Short diagnostic transfers do not need DMA. */
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        return err;
    }
    const spi_device_interface_config_t device = {
        .mode = 1,
        .clock_speed_hz = ADS131_SPI_CLOCK_HZ,
        .spics_io_num = ADS131_CS_PIN,
        .queue_size = 1,
        .cs_ena_pretrans = 1,
        .cs_ena_posttrans = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &device, &adc);
    if (err != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        return err;
    }
    /* Allow power-on settling before diagnostic register access. */
    vTaskDelay(pdMS_TO_TICKS(100) + 1);
    ESP_LOGI(TAG, "SPI2 ready: CS=%d MOSI=%d SCK=%d MISO=%d, mode=1, %d Hz",
             ADS131_CS_PIN, ADS131_MOSI_PIN, ADS131_SCK_PIN,
             ADS131_MISO_PIN, ADS131_SPI_CLOCK_HZ);
    ESP_LOGI(TAG, "SPI transport ready; acquisition setup will hardware-reset and configure ADC");
    return ESP_OK;
}

static esp_err_t write_checked(uint8_t address, uint16_t value)
{
    const uint16_t command = 0x6000u | ((uint16_t)address << 7);
    uint8_t tx[ADS131_FRAME_BYTES] = {
        command >> 8, command & 0xFF, 0,
        value >> 8, value & 0xFF, 0,
    };
    uint8_t rx[ADS131_FRAME_BYTES];
    esp_err_t err = exchange(tx, rx);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t ack;
    err = transfer_frame(0, &ack);
    if (err != ESP_OK) {
        return err;
    }
    if (ack != (0x4000u | ((uint16_t)address << 7))) {
        ESP_LOGE(TAG, "WREG 0x%02X unexpected ACK 0x%04X", address, ack);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t actual;
    err = ADS131_ReadReg(address, &actual);
    if (err != ESP_OK) {
        return err;
    }
    if (actual != value) {
        ESP_LOGE(TAG, "Register 0x%02X: wrote 0x%04X, read 0x%04X", address, value, actual);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "Configured addr=0x%02X value=0x%04X", address, actual);
    return ESP_OK;
}

esp_err_t ADS131_ConfigureAcquisition(void)
{
    if (adc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const gpio_config_t sync = {
        .pin_bit_mask = 1ULL << ADS131_SYNC_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&sync);
    if (err != ESP_OK) {
        return err;
    }
    /* Hardware reset restores framing even after an ESP-only reboot.
     * At 4.096 MHz, 2048 CLKIN periods = 500 us; hold low for 1 ms.
     */
    gpio_set_level(ADS131_SYNC_PIN, 0);
    esp_rom_delay_us(1000);
    gpio_set_level(ADS131_SYNC_PIN, 1);
    esp_rom_delay_us(1000);
    uint16_t id;
    err = ADS131_ReadReg(0, &id);
    if (err != ESP_OK) {
        return err;
    }
    if ((id & 0xFF00u) != 0x2400u) {
        ESP_LOGE(TAG, "Invalid ADS131M04 ID: 0x%04X", id);
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* UNLOCK; preserve 24-bit framing, restore acquisition settings explicitly.
     * CLOCK 0x0015: channels off, OSR=4096, LP mode for 4.096 MHz.
     * MODE 0x0110: CCITT CRC, RX CRC off, 24 bits, DRDY active-low level,
     * push-pull, most-lagging enabled channel, clear RESET indication.
     * MUX=00 selects external AINxP-AINxN. N-to-ground is board wiring.
     */
    err = transfer_frame(0x0655, NULL);
    if (err != ESP_OK) {
        return err;
    }
    static const struct { uint8_t address; uint16_t value; } config[] = {
        {0x03, 0x0015}, {0x02, 0x0110}, {0x04, 0x0000},
        {0x06, 0x0600}, {0x09, 0x0000}, {0x0E, 0x0000},
        {0x13, 0x0000}, {0x03, 0x0715},
    };
    for (size_t i = 0; i < sizeof(config) / sizeof(config[0]); ++i) {
        err = write_checked(config[i].address, config[i].value);
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGI(TAG, "AIN0..2 synchronous: CLKIN=4096000 Hz, OSR=4096, 500 SPS, gain=1");
    return ESP_OK;
}

void ADS131_Synchronize(void)
{
    /* 2 us > one CLKIN period but < reset threshold (500 us).
     * Prevent task/ISR preemption from extending this into a reset pulse.
     */
    portENTER_CRITICAL(&sync_lock);
    gpio_set_level(ADS131_SYNC_PIN, 0);
    esp_rom_delay_us(2);
    gpio_set_level(ADS131_SYNC_PIN, 1);
    portEXIT_CRITICAL(&sync_lock);
}

static uint16_t crc_ccitt(const uint8_t *data, size_t size)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1;
        }
    }
    return crc;
}

esp_err_t ADS131_ReadSample(ADS131_Sample *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t tx[ADS131_FRAME_BYTES] = {0};
    uint8_t rx[ADS131_FRAME_BYTES];
    esp_err_t err = exchange(tx, rx);
    if (err != ESP_OK) {
        return err;
    }
    /* Disabled CH3 still occupies a word; CRC covers response + all 4 words. */
    const uint16_t received_crc = ((uint16_t)rx[15] << 8) | rx[16];
    if (crc_ccitt(rx, 15) != received_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    sample->status = ((uint16_t)rx[0] << 8) | rx[1];
    if ((sample->status & 0xFF07u) != 0x0107u) {
        /* Require fresh CH0..2, 24-bit format and no reset/CRC/lock faults. */
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (unsigned ch = 0; ch < 3; ++ch) {
        const uint8_t *p = &rx[3 + ch * 3];
        uint32_t raw = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        sample->channel[ch] = (int32_t)(raw & 0x7FFFFFu) - (int32_t)(raw & 0x800000u);
    }
    return ESP_OK;
}

esp_err_t ADS131_ReadReg(uint8_t address, uint16_t *value)
{
    if (value == NULL || address > 0x3F) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* RREG = 101a aaaa annn nnnn; n=0 requests exactly one register.
     * The first frame receives the PREVIOUS command's response.
     * A second, NULL-command frame receives this register's value.
     */
    esp_err_t err = transfer_frame(ADS131_CMD_RREG | ((uint16_t)address << 7), NULL);
    if (err != ESP_OK) {
        return err;
    }
    return transfer_frame(0x0000, value);
}

esp_err_t ADS131_DumpRegisters(void)
{
    static const struct {
        uint8_t address;
        const char *name;
    } registers[] = {
        {0x00, "ID"}, {0x01, "STATUS"}, {0x02, "MODE"},
        {0x03, "CLOCK"}, {0x04, "GAIN"}, {0x06, "CFG"},
    };
    uint16_t id = 0;
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        uint16_t value;
        esp_err_t err = ADS131_ReadReg(registers[i].address, &value);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Read addr=0x%02X failed: %s",
                     registers[i].address, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "addr=0x%02X %-6s = 0x%04X",
                 registers[i].address, registers[i].name, value);
        if (i == 0) {
            id = value;
        }
    }
    /* ID low byte is reserved and may vary between silicon revisions. */
    if ((id & 0xFF00u) != 0x2400u) {
        ESP_LOGE(TAG, "Invalid ID 0x%04X (expected 0x24xx); SPI transfer alone does not prove ADC communication", id);
        ESP_LOGE(TAG, "Check ADC supplies/GND, CLKIN, SYNC/RESET high and SPI wiring; power-cycle ADC if word length/CRC was changed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "ADS131M04 ID matched (4 channels)");
    return ESP_OK;
}

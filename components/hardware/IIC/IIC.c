#include "IIC.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "IIC";
static i2c_master_bus_handle_t iic_bus;

esp_err_t IIC_Init(void)
{
    if (iic_bus != NULL) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = IIC_SDA_PIN,
        .scl_io_num = IIC_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&config, &iic_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Create I2C0 bus failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return err;
    }
    ESP_LOGI(TAG, "I2C0 ready: SCL=GPIO%d, SDA=GPIO%d, device clock=%d Hz",
             IIC_SCL_PIN, IIC_SDA_PIN, IIC_CLOCK_HZ);
    return ESP_OK;
}

esp_err_t IIC_AddDevice(uint8_t address, i2c_master_dev_handle_t *device)
{
    if (device == NULL || address < 0x08 || address > 0x77) {
        return ESP_ERR_INVALID_ARG;
    }
    if (iic_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2c_master_probe(iic_bus, address, IIC_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Probe 7-bit address 0x%02X failed: %s (0x%x)",
                 address, esp_err_to_name(err), (unsigned)err);
        /* Snapshot only: a high level does not prove adequate pull-ups/rise time. */
        ESP_LOGE(TAG, "Line levels after probe: SCL(GPIO%d)=%d, SDA(GPIO%d)=%d",
                 IIC_SCL_PIN, gpio_get_level(IIC_SCL_PIN),
                 IIC_SDA_PIN, gpio_get_level(IIC_SDA_PIN));
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "No address ACK; check PMIC supply, common GND, SDA/SCL routing and pull-ups");
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Bus timeout; check stuck-low lines and powered external pull-ups");
        }
        ESP_LOGW(TAG, "Probe is inconclusive; caller must verify with a register read");
    }
    return IIC_AddDeviceAtSpeed(address, IIC_CLOCK_HZ, device);
}

esp_err_t IIC_AddDeviceAtSpeed(uint8_t address, uint32_t clock_hz,
                              i2c_master_dev_handle_t *device)
{
    if (device == NULL || address < 0x08 || address > 0x77 || clock_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (iic_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = clock_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(iic_bus, &config, device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register device 0x%02X failed: %s (0x%x)",
                 address, esp_err_to_name(err), (unsigned)err);
    } else {
        ESP_LOGI(TAG, "Handle registered: address=0x%02X, clock=%lu Hz (not proof of ACK)",
                 address, (unsigned long)clock_hz);
    }
    return err;
}

esp_err_t IIC_Scan(void)
{
    if (iic_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    unsigned found = 0;
    ESP_LOGI(TAG, "Diagnostic address scan: 0x08..0x77 at 100 kHz");
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        esp_err_t err = i2c_master_probe(iic_bus, address, IIC_TIMEOUT_MS);
        if (err == ESP_OK) {
            ++found;
            ESP_LOGI(TAG, "Scan ACK at 7-bit address 0x%02X", address);
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Scan stopped at 0x%02X: %s", address, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "Scan complete: %u responding address(es)", found);
    return ESP_OK;
}

esp_err_t IIC_ReadReg(i2c_master_dev_handle_t device, uint8_t reg,
                      uint8_t *data, size_t length)
{
    if (device == NULL || data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_master_transmit_receive(device, &reg, 1, data, length, IIC_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read register 0x%02X failed: %s (0x%x)",
                 reg, esp_err_to_name(err), (unsigned)err);
    }
    return err;
}

esp_err_t IIC_WriteReg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t data[] = {reg, value};
    esp_err_t err = i2c_master_transmit(device, data, sizeof(data), IIC_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write register 0x%02X failed: %s (0x%x)",
                 reg, esp_err_to_name(err), (unsigned)err);
    }
    return err;
}

esp_err_t IIC_UpdateBits(i2c_master_dev_handle_t device, uint8_t reg,
                         uint8_t mask, uint8_t value)
{
    uint8_t current;
    esp_err_t err = IIC_ReadReg(device, reg, &current, 1);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t updated = (current & (uint8_t)~mask) | (value & mask);
    if (updated == current) {
        return ESP_OK;
    }
    err = IIC_WriteReg(device, reg, updated);
    if (err != ESP_OK) {
        return err;
    }
    err = IIC_ReadReg(device, reg, &current, 1);
    if (err != ESP_OK) {
        return err;
    }
    return (current & mask) == (value & mask) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

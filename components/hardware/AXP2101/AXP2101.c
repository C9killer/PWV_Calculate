#include "AXP2101.h"
#include "IIC.h"
#include "esp_log.h"

#define AXP2101_REG_STATUS         0x00
#define AXP2101_REG_COMMON         0x10
#define AXP2101_REG_POWER_SOURCE   0x20
#define AXP2101_REG_OFF_ENABLE     0x22
#define AXP2101_REG_KEY_TIMING     0x27
#define AXP2101_REG_DCDC_ENABLE    0x80
#define AXP2101_REG_DCDC1_VOLTAGE  0x82
#define AXP2101_REG_LDO_ENABLE     0x90
#define AXP2101_REG_ALDO1_VOLTAGE  0x92
#define AXP2101_REG_ALDO2_VOLTAGE  0x93

static i2c_master_dev_handle_t axp_device;
static bool axp_ready;
static const char *TAG = "AXP2101";

esp_err_t AXP2101_Init(void)
{
    if (axp_ready) {
        return ESP_OK;
    }
    /* A handle can survive a failed cleanup, but must never imply readiness. */
    if (axp_device != NULL) {
        esp_err_t cleanup = i2c_master_bus_rm_device(axp_device);
        if (cleanup != ESP_OK) {
            return cleanup;
        }
        axp_device = NULL;
    }
    esp_err_t err = IIC_Init();
    if (err != ESP_OK) {
        return err;
    }
    const uint32_t speeds[] = {IIC_CLOCK_HZ, 10000};
    for (unsigned attempt = 0; attempt < sizeof(speeds) / sizeof(speeds[0]); ++attempt) {
        err = attempt == 0 ? IIC_AddDevice(AXP2101_IIC_ADDRESS, &axp_device) :
              IIC_AddDeviceAtSpeed(AXP2101_IIC_ADDRESS, speeds[attempt], &axp_device);
        if (err != ESP_OK) {
            return err;
        }
        uint8_t status;
        ESP_LOGI(TAG, "Trying direct REG00 read at %lu Hz", (unsigned long)speeds[attempt]);
        err = IIC_ReadReg(axp_device, AXP2101_REG_STATUS, &status, 1);
        if (err == ESP_OK) {
            axp_ready = true;
            ESP_LOGI(TAG, "Communication established at %lu Hz: REG00=0x%02X",
                     (unsigned long)speeds[attempt], status);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Direct read at %lu Hz failed: %s",
                 (unsigned long)speeds[attempt], esp_err_to_name(err));
        esp_err_t cleanup = i2c_master_bus_rm_device(axp_device);
        if (cleanup != ESP_OK) {
            ESP_LOGE(TAG, "Device cleanup failed: %s", esp_err_to_name(cleanup));
            return cleanup;
        }
        axp_device = NULL;
        if (attempt == 0) {
            esp_err_t scan = IIC_Scan();
            if (scan != ESP_OK) {
                ESP_LOGW(TAG, "Scan incomplete; still trying one direct 10 kHz read");
            }
        }
    }
    ESP_LOGE(TAG, "No communication at 100/10 kHz; power configuration skipped");
    return err;
}

esp_err_t AXP2101_ReadStatus(AXP2101_Status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!axp_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t state[2];
    uint8_t source[2];
    esp_err_t err = IIC_ReadReg(axp_device, AXP2101_REG_STATUS, state, sizeof(state));
    if (err != ESP_OK) {
        return err;
    }
    err = IIC_ReadReg(axp_device, AXP2101_REG_POWER_SOURCE, source, sizeof(source));
    if (err == ESP_OK) {
        *status = (AXP2101_Status_t) {
            .power_status1 = state[0],
            .power_status2 = state[1],
            .power_on_source = source[0],
            .power_off_source = source[1],
        };
    }
    return err;
}

esp_err_t AXP2101_SetVoltage(AXP2101_Output_t output, uint16_t millivolts)
{
    uint8_t reg;
    uint16_t minimum;
    uint16_t maximum;
    switch (output) {
    case AXP2101_DCDC1:
        reg = AXP2101_REG_DCDC1_VOLTAGE;
        minimum = 1500;
        maximum = 3400;
        break;
    case AXP2101_ALDO1:
    case AXP2101_ALDO2:
        reg = output == AXP2101_ALDO1 ? AXP2101_REG_ALDO1_VOLTAGE : AXP2101_REG_ALDO2_VOLTAGE;
        minimum = 500;
        maximum = 3500;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (millivolts < minimum || millivolts > maximum || (millivolts - minimum) % 100 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!axp_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return IIC_UpdateBits(axp_device, reg, 0x1F, (millivolts - minimum) / 100);
}

esp_err_t AXP2101_SetOutput(AXP2101_Output_t output, bool enabled)
{
    uint8_t reg;
    uint8_t mask;
    switch (output) {
    case AXP2101_DCDC1:
        reg = AXP2101_REG_DCDC_ENABLE;
        mask = 0x01;
        break;
    case AXP2101_ALDO1:
    case AXP2101_ALDO2:
        reg = AXP2101_REG_LDO_ENABLE;
        mask = output == AXP2101_ALDO1 ? 0x01 : 0x02;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (!axp_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return IIC_UpdateBits(axp_device, reg, mask, enabled ? mask : 0);
}

esp_err_t AXP2101_SetKeyTiming(uint16_t on_ms, uint16_t long_press_ms,
                              uint16_t hardware_off_ms)
{
    uint8_t on;
    switch (on_ms) {
    case 128: on = 0; break;
    case 512: on = 1; break;
    case 1000: on = 2; break;
    case 2000: on = 3; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    if (long_press_ms < 1000 || long_press_ms > 2500 || long_press_ms % 500 != 0 ||
        hardware_off_ms < 4000 || hardware_off_ms > 10000 || hardware_off_ms % 2000 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!axp_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t timing = on | (((hardware_off_ms - 4000) / 2000) << 2) |
                     (((long_press_ms - 1000) / 500) << 4);
    return IIC_UpdateBits(axp_device, AXP2101_REG_KEY_TIMING, 0x3F, timing);
}

esp_err_t AXP2101_EnableHardwarePowerOff(void)
{
    if (!axp_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* REG22 table (p36): bit1 enables OFFLEVEL, bit0=0 selects off, not restart.
     * Keep thermal shutdown (bit2) and all other settings unchanged.
     */
    return IIC_UpdateBits(axp_device, AXP2101_REG_OFF_ENABLE, 0x03, 0x02);
}

esp_err_t AXP2101_PowerOff(void)
{
    if (!axp_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t value;
    esp_err_t err = IIC_ReadReg(axp_device, AXP2101_REG_COMMON, &value, 1);
    if (err != ESP_OK) {
        return err;
    }
    /* Bit1 is a reset command; never echo it back. No readback after power-off. */
    return IIC_WriteReg(axp_device, AXP2101_REG_COMMON, (value & (uint8_t)~0x02) | 0x01);
}

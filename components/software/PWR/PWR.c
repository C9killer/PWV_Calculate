#include "PWR.h"
#include <stdbool.h>
#include "AXP2101.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PWR";
static bool pwr_initialized;

esp_err_t PWR_Init(void)
{
    if (pwr_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(AXP2101_Init(), TAG, "AXP2101 communication failed");

    AXP2101_Status_t status;
    ESP_RETURN_ON_ERROR(AXP2101_ReadStatus(&status), TAG, "Read power status failed");
    ESP_LOGI(TAG, "Status=%02X/%02X, power-on=%02X, power-off=%02X",
             status.power_status1, status.power_status2,
             status.power_on_source, status.power_off_source);

    /* Configure hardware shutdown before enabling any peripheral rails.
     * PWROK already drives ESP32 EN: do not enable PWROK-low PMIC restart,
     * issue a PMIC reset, or change the factory cold-start sequence here.
     */
    ESP_RETURN_ON_ERROR(AXP2101_SetKeyTiming(PWR_KEY_ON_MS, PWR_KEY_LONG_PRESS_MS,
                                           PWR_KEY_HARDWARE_OFF_MS),
                        TAG, "Set key timing failed");
    ESP_RETURN_ON_ERROR(AXP2101_EnableHardwarePowerOff(), TAG, "Enable hardware shutdown failed");

    /* Never disable DCDC1: this rail powers the running ESP32. */
    ESP_RETURN_ON_ERROR(AXP2101_SetVoltage(AXP2101_DCDC1, PWR_DCDC1_MV),
                        TAG, "Set DCDC1 voltage failed");
    ESP_RETURN_ON_ERROR(AXP2101_SetOutput(AXP2101_DCDC1, true), TAG, "Enable DCDC1 failed");

    ESP_RETURN_ON_ERROR(AXP2101_SetVoltage(AXP2101_ALDO1, PWR_ALDO1_MV),
                        TAG, "Set ALDO1 voltage failed");
    ESP_RETURN_ON_ERROR(AXP2101_SetVoltage(AXP2101_ALDO2, PWR_ALDO2_MV),
                        TAG, "Set ALDO2 voltage failed");
    ESP_RETURN_ON_ERROR(AXP2101_SetOutput(AXP2101_ALDO1, true), TAG, "Enable ALDO1 failed");
    ESP_RETURN_ON_ERROR(AXP2101_SetOutput(AXP2101_ALDO2, true), TAG, "Enable ALDO2 failed");

    /* Board-level settling allowance, not a PMIC power-good measurement.
     * Allow at least one tick even if the RTOS tick period exceeds 10 ms.
     */
    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    pwr_initialized = true;
    ESP_LOGI(TAG, "DCDC1=3300mV, ALDO1=3300mV, ALDO2=1600mV");
    ESP_LOGI(TAG, "Key: on=512ms, hardware off=6s; no software key handler");
    return ESP_OK;
}

esp_err_t PWR_PowerOff(void)
{
    if (!pwr_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Requesting PMIC power-off");
    return AXP2101_PowerOff();
}

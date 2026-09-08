#ifndef AXP2101_H_
#define AXP2101_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AXP2101_IIC_ADDRESS 0x34

typedef enum {
    AXP2101_DCDC1,
    AXP2101_ALDO1,
    AXP2101_ALDO2,
} AXP2101_Output_t;

typedef struct {
    uint8_t power_status1;
    uint8_t power_status2;
    uint8_t power_on_source;
    uint8_t power_off_source;
} AXP2101_Status_t;

/* Single owner, task context only. PWR is the board-level owner.
 * Init only establishes communication; it does not change any power outputs.
 */
esp_err_t AXP2101_Init(void);
esp_err_t AXP2101_ReadStatus(AXP2101_Status_t *status);
/* Exact millivolts only. Unsupported values (including ALDO2=1650) are rejected. */
esp_err_t AXP2101_SetVoltage(AXP2101_Output_t output, uint16_t millivolts);
esp_err_t AXP2101_SetOutput(AXP2101_Output_t output, bool enabled);
/* on: 128/512/1000/2000; long press: 1000/1500/2000/2500;
 * hardware off: 4000/6000/8000/10000 ms. Does not enable IRQ events.
 */
esp_err_t AXP2101_SetKeyTiming(uint16_t on_ms, uint16_t long_press_ms,
                              uint16_t hardware_off_ms);
esp_err_t AXP2101_EnableHardwarePowerOff(void);
/* Last operation after application shutdown preparation; power may disappear
 * before the I2C transaction returns. Never reset/retry the PMIC automatically.
 */
esp_err_t AXP2101_PowerOff(void);

#endif

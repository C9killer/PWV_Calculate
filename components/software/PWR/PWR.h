#ifndef PWR_H_
#define PWR_H_

#include "esp_err.h"

#define PWR_DCDC1_MV 3300
#define PWR_ALDO1_MV 3300
#define PWR_ALDO2_MV 1600

#define PWR_KEY_ON_MS 512
/* Only programs the threshold; key IRQ events are not configured or polled. */
#define PWR_KEY_LONG_PRESS_MS 1500
#define PWR_KEY_HARDWARE_OFF_MS 6000

/* Call once from app_main before initializing peripherals.
 * DCDC1 must already power the ESP32 at a safe voltage at cold boot.
 * Runtime settings do not program the PMIC's factory/EFUSE defaults.
 * All PWR/AXP2101 calls must be serialized by the application, in task context.
 */
esp_err_t PWR_Init(void);
/* Save data and stop peripherals before calling. No automatic button handler. */
esp_err_t PWR_PowerOff(void);

#endif

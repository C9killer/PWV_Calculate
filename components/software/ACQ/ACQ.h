#ifndef ACQ_H
#define ACQ_H

#include "esp_err.h"

#define ACQ_DRDY_PIN 7
#define ACQ_SAMPLE_RATE_HZ 250

/* Start once from app_main after PWR_Init. Owns all subsequent ADC access.
 * SYNC/RESET=GPIO6 output; DRDY=GPIO7 falling-edge interrupt input.
 * Sends synchronous AIN0..2 CSV frames over UART and the Wi-Fi data service.
 */
esp_err_t ACQ_Start(void);

#endif

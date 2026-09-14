#ifndef ACQ_H
#define ACQ_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ACQ_DRDY_PIN 7
#define ACQ_SAMPLE_RATE_HZ 500
#define ACQ_WINDOW_SECONDS 5U
#define ACQ_WINDOW_SAMPLE_COUNT (ACQ_SAMPLE_RATE_HZ * ACQ_WINDOW_SECONDS)

typedef struct {
    uint32_t first_sequence;
    uint32_t last_sequence;
    size_t sample_count;
    int32_t sample[ACQ_WINDOW_SAMPLE_COUNT][3];
} ACQ_Window;

/* Start once from app_main after PWR_Init. Owns all subsequent ADC access.
 * SYNC/RESET=GPIO6 output; DRDY=GPIO7 falling-edge interrupt input.
 * Sends synchronous AIN0..2 CSV frames over UART and the Wi-Fi data service.
 */
esp_err_t ACQ_Start(void);

/* Called by the window worker for every contiguous five-second window.
 * The default weak implementation does nothing. The signal/model component
 * can provide a strong implementation. The pointer is valid only until this
 * function returns; finish within five seconds to avoid losing a window.
 */
void ACQ_ProcessWindow(const ACQ_Window *window);

#endif

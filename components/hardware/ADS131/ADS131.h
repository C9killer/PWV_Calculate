#ifndef ADS131_H
#define ADS131_H

#include <stdint.h>
#include "esp_err.h"

#define ADS131_CS_PIN       10
#define ADS131_MOSI_PIN     11
#define ADS131_SCK_PIN      12
#define ADS131_MISO_PIN     13
#define ADS131_SYNC_PIN     6
#define ADS131_SPI_CLOCK_HZ 1000000

/* ADS131M04 protocol: 24-bit words, input CRC disabled.
 * CLKIN must be supplied by the board; SYNC/RESET is driven on GPIO6.
 * Call from one task only (a register read spans two SPI transactions).
 */
esp_err_t ADS131_Init(void);
esp_err_t ADS131_ReadReg(uint8_t address, uint16_t *value);
esp_err_t ADS131_DumpRegisters(void);

typedef struct {
    uint16_t status;
    int32_t channel[3]; /* Signed 24-bit codes, AIN0..2 from the same frame. */
} ADS131_Sample;

/* Configure 4.096 MHz CLKIN / 2 / 4096 = 500 samples/s, gain=1.
 * Continuous conversion, equal channel phase, CH0..2 enabled, CH3 disabled.
 * After configuration, only the acquisition task may access the ADC.
 */
esp_err_t ADS131_ConfigureAcquisition(void);
esp_err_t ADS131_ReadSample(ADS131_Sample *sample);
/* Clear conversion FIFOs and align all channels; discard settling samples. */
void ADS131_Synchronize(void);

#endif

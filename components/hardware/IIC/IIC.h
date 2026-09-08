#ifndef IIC_H_
#define IIC_H_

#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#define IIC_SCL_PIN GPIO_NUM_4
#define IIC_SDA_PIN GPIO_NUM_5
#define IIC_CLOCK_HZ 100000
#define IIC_TIMEOUT_MS 100

/* Task context only. Initialize/add devices before starting client tasks.
 * Serialize read-modify-write operations on the same device across tasks.
 * SDA/SCL need external pull-ups to the 3.3 V logic supply (datasheet: 2.2k).
 */
esp_err_t IIC_Init(void);
/* Probe is advisory: ESP_OK here means a handle was registered, not a device ACK.
 * The caller must verify communication with a read before configuring hardware.
 */
esp_err_t IIC_AddDevice(uint8_t address, i2c_master_dev_handle_t *device);
/* Register without probing, for a real read transaction at the requested speed. */
esp_err_t IIC_AddDeviceAtSpeed(uint8_t address, uint32_t clock_hz,
                              i2c_master_dev_handle_t *device);
/* Diagnostic address-only scan (0x08..0x77), fixed 100 kHz in ESP-IDF 6.1.
 * Stops on bus errors; never selects a device or writes register data.
 */
esp_err_t IIC_Scan(void);
esp_err_t IIC_ReadReg(i2c_master_dev_handle_t device, uint8_t reg,
                      uint8_t *data, size_t length);
esp_err_t IIC_WriteReg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value);
/* For ordinary RW registers only, never W1C or command registers. Verifies readback. */
esp_err_t IIC_UpdateBits(i2c_master_dev_handle_t device, uint8_t reg,
                         uint8_t mask, uint8_t value);

#endif

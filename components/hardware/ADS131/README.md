# ADS131M04 driver

SPI2: CS=GPIO10, DIN/MOSI=GPIO11, SCLK=GPIO12, DOUT/MISO=GPIO13.
SYNC/RESET=GPIO6 (output); acquisition DRDY=GPIO7 (input, handled by ACQ).
The board supplies a continuous 4.096 MHz CLKIN. SPI uses mode 1, 1 MHz,
24-bit words, input CRC disabled, and complete 18-byte frames.

ADS131_ConfigureAcquisition() hardware-resets the ADC with a 1 ms low pulse,
checks ID=0x24xx, and writes/verifies these registers:

| Register | Value | Meaning |
| --- | --- | --- |
| CLOCK (0x03) | 0x0719 | CH0..2 enabled, CH3 disabled, OSR=8192, low-power mode |
| MODE (0x02) | 0x0110 | 24-bit words, CCITT CRC, DRDY active-low level, RESET cleared |
| GAIN (0x04) | 0x0000 | Gain=1 for all channels |
| CFG (0x06) | 0x0600 | Continuous conversion, global chop disabled |
| CH0/1/2_CFG (0x09/0x0E/0x13) | 0x0000 | Equal zero phase, external P/N inputs |

CLOCK is first set to 0x0019 to disable channels during configuration. The
resulting per-channel rate is 4096000 / 2 / 8192 = 250 samples/s.
Pseudo-differential operation is physical wiring: AINxP receives the signal,
AINxN connects to analog ground. Gain=1 differential full scale is +/-1.2 V;
raw codes are signed 24-bit, nominal volts = code * 1.2 / 8388608.

ADS131_ReadSample() sends NULL, checks the output CRC (CCITT, seed 0xFFFF,
polynomial 0x1021), checks status/freshness, and decodes CH0..2 from one frame.
Disabled CH3 still occupies its word before the CRC. ADS131_Synchronize()
provides a 2 us low pulse that clears FIFOs and aligns conversion timing.

All APIs run in task context and require serialized access. Once ACQ starts,
do not call register dumps or other SPI access from app_main or another task.
See ../../software/ACQ/README.md for startup and test output.

Reference: supplied ADS131M04 datasheet, sections 8.3.12, 8.5.1.7-10,
8.5.2, and register descriptions in 8.6.

# W25Q64JV external Flash

The board connects the independent 8 MiB W25Q64JV to ESP32-S3 SPI3:

| Signal | GPIO |
| --- | ---: |
| MISO / DO | 42 |
| MOSI / DI | 41 |
| CLK | 40 |
| CS | 48 |

ADS131M04 uses SPI2, so ADC acquisition and external-Flash traffic are on
independent peripheral buses. GPIO48 is also assigned to the existing WS2812
driver; do not call `LED_Init()` while this Flash connection is populated.

Call `W25Q64_Init()` before other APIs. Initialization releases power-down,
resets the chip, and requires JEDEC ID `EF 40 17`. Reads are split into 4 KiB
SPI transactions and writes are split on 256-byte page boundaries. Call
`W25Q64_EraseSector()` before programming data that requires any bit to change
from 0 back to 1.

Example:

```c
uint8_t model_header[64];

ESP_ERROR_CHECK(W25Q64_Init());
ESP_ERROR_CHECK(W25Q64_Read(0, model_header, sizeof(model_header)));
```

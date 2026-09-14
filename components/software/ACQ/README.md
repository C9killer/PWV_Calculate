# Three-channel acquisition

Call `ACQ_Start()` after `PWR_Init()`; app_main already does this. The existing
software CMake file discovers the ACQ directory automatically after reconfigure.

- External ADC CLKIN: 4.096 MHz; each channel samples at 500 Hz.
- GPIO6 drives ADS131M04 SYNC/RESET. GPIO7 receives DRDY falling edges.
- AIN0P..AIN2P receive signals; their N inputs connect to analog ground.
- ISR only gives a task notification. The priority-10 acquisition task reads
  CH0..2 together in one SPI frame and verifies CRC and fresh-data status.
- Two five-second buffers (2500 synchronous frames each) are allocated in
  PSRAM. The acquisition task fills one while a priority-4 worker passes the
  other to `ACQ_ProcessWindow()` for feature extraction and inference.
- Applications provide a strong implementation of the weak
  `ACQ_ProcessWindow()` hook. Processing must finish within five seconds;
  otherwise incoming samples are dropped until a PSRAM window is returned.
- Every valid frame is also copied nonblockingly to a 256-entry stream queue.
  A priority-3 worker formats CH0..2 as CSV and sends the same line to UART
  and the Wi-Fi TCP service. Slow outputs never block the acquisition task;
  queue overflow is reported as `stream_drops`.
- Startup sends SYNC and drains/discards four conversion frames for settling.
  Multiple pending notifications or invalid samples cause resynchronization
  and another settling interval. Missed samples are not reconstructed.
- No DRDY for one second produces a wiring/clock diagnostic. Error counters
  are reported at most once per second.

Raw samples and startup diagnostics share the 921600-baud console. Each raw
line contains three signed decimal ADC codes separated by commas. The same
CSV line is sent to the TCP client when connected.

Build and test from an ESP-IDF terminal:

```text
idf.py reconfigure build
idf.py -p COM22 flash monitor
```

Close any other monitor holding COM22. Compilation succeeded; the automated
upload attempt received access denied on COM22, so actual DRDY frequency,
register readback, CRC and sample output still require hardware
verification. Expect approximately 500 synchronous sample frames/s after settling
with no error counters; check DRDY periods of about 2 ms with a logic analyzer if necessary.

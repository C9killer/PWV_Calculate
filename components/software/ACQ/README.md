# Three-channel acquisition

Call `ACQ_Start()` after `PWR_Init()`; app_main already does this. The existing
software CMake file discovers the ACQ directory automatically after reconfigure.

- External ADC CLKIN: 4.096 MHz; each channel samples at 250 Hz.
- GPIO6 drives ADS131M04 SYNC/RESET. GPIO7 receives DRDY falling edges.
- AIN0P..AIN2P receive signals; their N inputs connect to analog ground.
- ISR only gives a task notification. The priority-10 acquisition task reads
  CH0..2 together in one SPI frame and verifies CRC and fresh-data status.
- Two five-second buffers (1250 synchronous frames each) are allocated in
  PSRAM. The acquisition task fills one while a priority-4 worker passes the
  other to `ACQ_ProcessWindow()` for feature extraction and inference.
- Applications provide a strong implementation of the weak
  `ACQ_ProcessWindow()` hook. Processing must finish within five seconds;
  otherwise incoming samples are dropped until a PSRAM window is returned.
- Every valid frame is also copied nonblockingly to the 384-entry protocol queue.
  A priority-3 worker sends CH0..2 as three identical 10-byte float32 protocol
  frames over both UART and the Wi-Fi TCP service. Slow outputs never block
  the acquisition task; queue overflow is reported as `protocol_drops`.
- Startup sends SYNC and drains/discards four conversion frames for settling.
  Multiple pending notifications or invalid samples cause resynchronization
  and another settling interval. Missed samples are not reconstructed.
- No DRDY for one second produces a wiring/clock diagnostic. Error counters
  are reported at most once per second.

Binary samples and diagnostics share the 921600-baud console. UART and TCP
receive identical ECG/PPG1/PPG2 frames and valid SBP/DBP prediction frames:
`FF 09 03 ID 01 <float32 little-endian, 4 bytes> AA`. UART and native USB
Serial/JTAG newline conversion is disabled to preserve binary payloads.
Native USB uses the interrupt driver with a 4096-byte TX buffer, which flushes
short packets without waiting for a newline or a full USB packet. The 921600
baud setting applies to physical UART0, not native USB transfer speed.
The serial receiver must skip text
diagnostics and parse protocol frames; samples are no longer CSV text.

Build and test from an ESP-IDF terminal:

```text
idf.py reconfigure build
idf.py -p COM22 flash monitor
```

Close any other monitor holding COM22. Compilation succeeded; the automated
upload attempt received access denied on COM22, so actual DRDY frequency,
register readback, CRC and sample output still require hardware
verification. Expect approximately 250 synchronous sample frames/s after settling
with no error counters; check DRDY periods of about 4 ms with a logic analyzer if necessary.

At 250 Hz, each transport carries 750 ten-byte sample packets/s (7,500
bytes/s, excluding diagnostics and BP packets). Configure the host waveform
time axis for 250 Hz. The BP branch submits every synchronized frame directly
to the 250 Hz model frontend without bridge low-pass filtering or decimation,
retaining five-second model windows (1250 samples per channel).

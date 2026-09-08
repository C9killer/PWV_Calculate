# Three-channel acquisition

Call `ACQ_Start()` after `PWR_Init()`; app_main already does this. The existing
software CMake file discovers the ACQ directory automatically after reconfigure.

- External ADC CLKIN: 4.096 MHz; each channel samples at 250 Hz.
- GPIO6 drives ADS131M04 SYNC/RESET. GPIO7 receives DRDY falling edges.
- AIN0P..AIN2P receive signals; their N inputs connect to analog ground.
- ISR only gives a task notification. The priority-10 acquisition task reads
  CH0..2 together in one SPI frame and verifies CRC and fresh-data status.
- A separate priority-3 task prints through a 64-entry queue. Slow serial
  output cannot block SPI acquisition; a full queue drops the print record
  and increments `print_drops`.
- Startup sends SYNC and drains/discards four conversion frames for settling.
  Multiple pending notifications or invalid samples cause resynchronization
  and another settling interval. Missed samples are not reconstructed.
- No DRDY for one second produces a wiring/clock diagnostic. Error counters
  are reported at most once per second.

Console: 115200 baud. Each sample line contains only the signed raw AIN0
code, not volts, with no header or sequence number. All three channels are
still acquired synchronously. Startup logs and occasional diagnostics share
the console; a host parser should accept only lines containing one integer.

Build and test from an ESP-IDF terminal:

```text
idf.py reconfigure build
idf.py -p COM22 flash monitor
```

Close any other monitor holding COM22. Compilation succeeded; the automated
upload attempt received access denied on COM22, so actual DRDY frequency,
register readback, CRC and sample output still require hardware
verification. Expect approximately 250 AIN0 values/s after settling with no error
counters; check DRDY periods of about 4 ms with a logic analyzer if necessary.

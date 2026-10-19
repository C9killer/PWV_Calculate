# Internal model partition reader

`ModelStorage_Init()` locates the `xgbmodel` data partition declared in the
project's `partitions.csv`. `ModelStorage_ReadChunk()` provides bounded reads
for a reusable model cache; the default recommended cache size is 16 KiB.

The reader intentionally does not interpret XGBoost JSON, UBJSON, or a custom
tree format. The model converter must generate a compact format with a header,
payload length, CRC, feature-schema identifier, tree index, and tree records.
The inference layer should read the header and index once, then request only
the tree blocks needed for traversal. Scanning the complete 10 MiB partition
for every prediction is not an acceptable use of this API.

## WKESP1 strict package integration

`wk_esp32_partition.*` and `wk_edge_inference.*` attach the supplied strict
float64 `WKESP1` package to the same `xgbmodel` partition. The partition
contains 12 MiB; the accepted `mimic_iii_wk_mk_f64.bin` uses 9,341,904 bytes.
Each prediction maps one model payload (maximum 2.50 MiB) and releases it
immediately; the model is never copied to PSRAM. Persistent inference state
uses about 7.4 KiB, leaving the 8 MiB PSRAM to acquisition and feature buffers.

After flashing firmware, provision the partition (replace `COMx`):

```powershell
python $env:IDF_PATH/components/partition_table/parttool.py --port COMx write_partition --partition-name xgbmodel --input E:\PWV\Project\TIM_Hardware\outputs\mimic_iii_wk_mk_f64.bin
```

Generate the 185 features in exporter-defined compact order, call
`wk_edge_predict_rc()`, form the six-element mechanism vector, then call
`wk_edge_predict_bp()`. The first window establishes previous-feature state
and has no difference-model result. The current acquisition component does
not yet contain the 185-feature extractor; startup only validates and attaches
a provisioned package.

## Real-time causal frontend

The current deployment includes the newer causal 250 Hz frontend: filters,
rolling five-second windows (one-second hop), feature extraction, fitted
feature-domain adaptation, calibration and BP fusion. The ADS131 driver is
configured for synchronized 250 Hz frames. `WKRealtimeBridge` submits every
frame directly to the 250 Hz frontend, bypassing its optional anti-alias filter
and decimation. The frontend's causal filters remain active. USB and Wi-Fi
retain every raw 250 Hz frame; each five-second window has 1250 samples per
channel.

Default physical channel mapping is **AIN0 = ECG, AIN1 = fingertip PPG,
AIN2 = wrist PPG**, and both PPG polarities are positive. Confirm the wiring
and change the three `WK_ACQ_*_CHANNEL` constants or PPG polarity arguments
in `WKRealtimeBridge.c` before interpreting BP results. The model task emits
one result per second after its initial five-second window, subject to signal
quality checks. Use `WKRealtimeBridge_SubmitReference(sbp, dbp)` to provide a
cuff reading for the next inference window.

## Resource and runtime profiling

`WKRealtimeBridge` prints byte counts at startup (`RESOURCE`) and a `PROFILE`
line for each five-second window, with a one-second hop:

- `model_flash_B`: actual WKESP1 package length read from its header (all eight
  ensembles). `partition_reserved_B` is the reserved capacity, not model size.
  Algorithm code and constants in the app image are separate; inspect the
  firmware map or `idf.py size-files` for these.
- `pipeline_psram_B`: the entire persistent algorithm state including signal
  buffers, feature workspace and edge state. `edge_state_included_B` is a
  subset, so do not add it again. Queue payload and reserved task stack are
  reported separately; these exclude allocator metadata and RTOS control blocks.
- `stack_peak_since_start_B`: task lifetime stack high-water estimate, including
  reporting and publishing. It is not reset per inference. Reserved stack is
  already allocated RAM; do not add stack usage to reserved stack again.
- `system_window ...used_peak_bound_B`: total heap capacity minus the local
  minimum free heap during this window, separated into internal RAM and PSRAM.
  `...extra_peak_bound_B` is the decrease from free heap before the call.
  The ESP-IDF local monitor captures transient allocation lows, including model
  mapping bookkeeping. It is system-wide, so Wi-Fi/acquisition allocations are
  included. Region minima may occur at different instants, making these upper
  bounds, not exact simultaneous peaks or algorithm-exclusive RAM measurements.
  Static firmware RAM outside the heap is not included. The model is Flash mapped,
  not copied into a model-sized PSRAM buffer. If another local heap monitor is
  active, this profiler skips memory measurement and leaves that monitor running.
- `time_us`: wall time around `wk_realtime_push_sample` on the window-producing
  sample, covering snapshot, feature extraction, model inference and BP processing.
  It excludes profiler logs and network publication, but includes scheduling
  preemption. It excludes filtering the preceding 249 samples of the hop.
  `avg_us`, `max_us`, `n` and `over_hop` accumulate separately per status since
  startup. `over_hop` counts calls exceeding the one-second hop.
- `latency_us`: time from submission of the last sample to computation completion,
  including queue wait (not ADC conversion, five-second acquisition or transmission).
  `queue` reports backlog at profiling time.

For measurement, flash this firmware and capture serial output during representative
signals. Record `RESOURCE`, then report `PROFILE status=output_ready` independently
of rejected/error windows; retain the first valid window because later windows also
run difference models. Run enough windows to capture steady-state maxima. A call
below one second alone does not prove sustained real-time performance: also check
queue backlog, end-to-end latency and existing sample-drop warnings. RAM and runtime
numbers must come from the device; compilation alone cannot supply measured peaks.

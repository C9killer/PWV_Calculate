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

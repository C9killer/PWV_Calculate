# Model files

This directory separates PC training artifacts from the binary consumed by
the ESP32-S3 firmware.

```text
models/
|-- source/                 Original PC-side trained models
|-- device/                 Converted files downloaded to xgbmodel
|   |-- model.bin           Generated model package (not tracked by Git)
|   `-- model_manifest.json Human-readable metadata for model.bin
`-- tools/                  PC-side model conversion and validation tools
```

Do not copy `.pkl`, `.joblib`, XGBoost JSON/UBJSON, or LightGBM text files
directly to the device partition. A converter in `tools/` should generate a
bounded, CRC-protected `device/model.bin` that supports indexed block reads.

The current internal-Flash destination is the `xgbmodel` partition declared
in `../partitions.csv`: offset `0x210000`, capacity `0xC00000` (12 MiB).


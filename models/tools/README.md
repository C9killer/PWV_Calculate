# Model conversion tools

Place PC-side scripts here. The planned converter should:

1. load an XGBoost or LightGBM training artifact;
2. validate the exact feature order and preprocessing metadata;
3. convert trees to the common compact device format;
4. write a header, tree-offset index, tree blocks, and CRC32;
5. emit `../device/model.bin` and `../device/model_manifest.json`;
6. reject output larger than the 12 MiB `xgbmodel` partition.

The converter will be implemented after the final source-model format and
feature schema are available.


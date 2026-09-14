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

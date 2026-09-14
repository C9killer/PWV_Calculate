# Device model package

The PC-side converter writes the deployable package here:

```text
model.bin
model_manifest.json
```

`model.bin` must not exceed `0xC00000` bytes. The project-wide `.gitignore`
already excludes `*.bin`. Keep `model_manifest.json` beside the binary so its
model version, framework, feature schema, size, and CRC can be audited.


# Streaming 20GB Codebook Sweep

`fpc-bsel.v2` provides `train-budget-stream`, `evaluate-stream`, and
`roundtrip-stream`. Input is read in fixed chunks and memory use does not grow
with file size.

Prefix patterns cost 16B and residual patterns cost 64B. Budgeted training
ranks both families by compression gain per table byte and enforces one shared
1/2/3/4KiB hardware budget. Serialized model files add a fixed 28B header.

Run the complete sweep with:

```powershell
powershell -File tools/run_fpc_20g_stream.ps1 `
  -Train datasets/large/train-20g.bin `
  -Test datasets/large/test-20g.bin `
  -OutputDir results/fpc-20g `
  -ChunkMiB 256 -RoundTrip -RunMcc
```

For one unsplit dataset, use a logical aligned split without copying the file:

```powershell
powershell -File tools/run_fpc_20g_stream.ps1 `
  -Input datasets/large/dataset-20g.bin `
  -TrainPercent 20 `
  -OutputDir results/fpc-20g `
  -ChunkMiB 256 -RoundTrip -RunMcc
```

The split point is rounded down to a 256B subline boundary. The prefix range is
used only for training and the suffix range only for evaluation and MCC.

Use `-Resume` after interruption. Completed models and evaluation logs are
reused. `-RunMcc` processes the test file chunk by chunk, resets MCC at each
chunk boundary, and records the number of boundaries so their conservative
padding overhead is visible.

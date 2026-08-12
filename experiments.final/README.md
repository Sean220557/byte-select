# Final Compression and MCC Experiment

`tools/run_final_compression_mcc.ps1` accepts one unsplit dataset. The prefix
range is used for training and the suffix range for testing; the split is
aligned to 256B and the input file is not copied.

The script trains and evaluates FPC-BSEL with shared prefix/residual codebook
budgets of 4KiB, 3KiB, 2KiB, and 1KiB. It also evaluates the built-in FPC,
BDI, hybrid FPC/BDI, C-Pack, BPC, and Huffman baselines. Every algorithm emits
real stored sizes for 256B sublines and is evaluated through MCC before packing,
v1, v2, v3, v4, and v5. The recommended v5 point uses a 1B guard and one-region
lookback.

The selected ablation algorithm defaults to FPC-BSEL 3KiB. It is additionally
evaluated with guard 0/1 and region lookback 0/1/2. Candidate segment and gap
checks are recorded alongside quantized ratios.

```powershell
powershell -File tools/run_final_compression_mcc.ps1 `
  -Input datasets/large/dataset-20g.bin `
  -TrainPercent 20 `
  -OutputDir results/final-20g `
  -ChunkMiB 256 `
  -RoundTrip `
  -Resume
```

`final-summary.csv` contains the cross-algorithm result and
`mcc-ablation-fpc-bsel-3k.csv` contains the MCC ablation. Each chunk resets MCC,
so chunk-boundary padding is conservatively charged. Larger chunks reduce this
boundary effect. `-Resume` reuses completed FPC models and evaluation logs;
the final MCC pass is rerun from the start.

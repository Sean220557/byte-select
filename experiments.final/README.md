# Final Compression and MCC Experiment

`tools/run_single_trace_all_codecs.sh` accepts one unsplit dataset. The prefix
range is used for training and the suffix range for testing; the split is
aligned to 256B and the input file is not copied.

The script trains and evaluates FPC-BSEL with shared prefix/residual codebook
budgets of 4KiB, 3KiB, 2KiB, and 1KiB. It also evaluates the built-in FPC,
BDI, hybrid FPC/BDI, C-Pack, BPC, and Huffman baselines. Every algorithm emits
real stored sizes for 256B sublines and is evaluated through MCC before packing,
v1, v2, v3, v4, and v5. The recommended v5 point uses a 1B guard and one-region
lookback.

All algorithms use the same 256B decision and raw-fallback granularity. FPC-BSEL
retains four internal 64B FPC lanes for implementation parallelism, but the
four lane modes are packed into one shared byte, the lane payloads are joined,
and raw fallback is selected once for the complete 256B subline. Therefore its
stored size is `min(256, 1 + sum(four lane payloads))`, rather than the older
sum of four independently selected 64B records. `sizes-256 --roundtrip` checks
every internal lane while generating these sizes.

The selected ablation algorithm defaults to FPC-BSEL 3KiB. It is additionally
evaluated with guard 0/1 and region lookback 0/1/2. Candidate segment and gap
checks are recorded alongside quantized ratios.

`final-summary.csv` contains the cross-algorithm result and
`mcc-ablation-fpc-bsel-3k.csv` contains the MCC ablation. Each chunk resets MCC,
so chunk-boundary padding is conservatively charged. Larger chunks reduce this
boundary effect. `-Resume` reuses completed FPC models and evaluation logs;
the final MCC pass is rerun from the start.

## Linux

The native Bash runner has no PowerShell dependency:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake -S fpc-bsel.v2 -B fpc-bsel.v2/build -DCMAKE_BUILD_TYPE=Release
cmake --build fpc-bsel.v2/build -j

chmod +x tools/run_single_trace_all_codecs.sh
TRAIN_PERCENT=20 CHUNK_MIB=256 ROUNDTRIP=1 RESUME=1 \
  tools/run_single_trace_all_codecs.sh \
  /data/dataset-20g.bin results/final-20g
```

The Linux runner uses GNU `stat`, `dd`, and `awk`. It streams the test range in
chunks and does not create a second full-size dataset copy.

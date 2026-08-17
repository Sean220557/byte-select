# Final Compression and MCC Experiment

`tools/run_single_trace_all_codecs.sh` accepts one unsplit dataset. The prefix
range is used for training and the suffix range for testing; the split is
aligned to 256B and the input file is not copied.

The default script evaluates the four FPC-BSEL codebook budgets, FPC Top-256,
BDI, hybrid FPC-Top-256/BDI, C-Pack, BPC, and Huffman. Pure Byte Select is not
included. Every algorithm emits real stored sizes for 256B sublines and is
evaluated through MCC before packing, v1, v2, v3, v4, and v5.

All enabled algorithms use the same 256B decision and raw-fallback granularity.
The FPC Top-256 dictionary contains the 256 most frequent 32-bit residual words.
It is trained only on the training prefix and then reused for every test chunk.
Each residual word is represented by an 8-bit index on a hit; misses remain
literal. The fallback compares word-level Top-256 FPC against regular and
BitShuffle FPC. The hybrid additionally compares against BDI. This word-level
version directly replaces the older whole-residual-block Top-256 format.
Within each 4KiB region, a 16-entry FIFO residual-word dictionary provides
4-bit local indices ahead of the global dictionary. The FPC tag stream also
selects between literal 3-bit tags and run-length encoded tag runs; one mode
bit is charged. These are FPC-native candidates and do not invoke C-Pack.

The `fpc-top256-xor` and `hybrid-top256-xor` rows additionally exploit spatial
locality within each 4KiB region. The first 256B subline is an anchor; later
sublines compare previous-XOR, region-anchor-XOR, and previous-word-delta
representations. Dependencies reset every 16 sublines. A spatial encoding is
selected only when it remains smaller after charging one byte of mode/reference
metadata. The non-XOR rows are retained as the algorithm-side ablation baseline.

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
chmod +x tools/run_single_trace_all_codecs.sh
TRAIN_PERCENT=20 CHUNK_MIB=256 ROUNDTRIP=1 RESUME=1 \
  tools/run_single_trace_all_codecs.sh \
  /data/dataset-20g.bin results/final-20g
```

The Linux runner uses GNU `stat`, `dd`, and `awk`. It streams the test range in
chunks and does not create a second full-size dataset copy.

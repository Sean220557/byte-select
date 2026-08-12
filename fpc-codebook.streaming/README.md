# Streaming 20GB Codebook Sweep

`fpc-bsel.v2` provides `train-budget-stream`, `evaluate-stream`, and
`roundtrip-stream`. Input is read in fixed chunks and memory use does not grow
with file size.

Prefix patterns cost 16B and residual patterns cost 64B. Budgeted training
ranks both families by compression gain per table byte and enforces one shared
1/2/3/4KiB hardware budget. Serialized model files add a fixed 28B header.

Run the complete sweep on one unsplit dataset:

```bash
TRAIN_PERCENT=20 CHUNK_MIB=256 ROUNDTRIP=1 RESUME=1 \
  tools/run_single_trace_all_codecs.sh \
  /data/dataset-20g.bin results/fpc-20g
```

The split point is rounded down to a 256B subline boundary. The prefix range is
used only for training and the suffix range only for evaluation and MCC.

Use `RESUME=1` after interruption. Completed models and evaluation logs are
reused. The runner processes the test file chunk by chunk, resets MCC at each
chunk boundary, and records the number of boundaries so their conservative
padding overhead is visible.

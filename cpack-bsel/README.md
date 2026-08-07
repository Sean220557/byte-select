# Standalone FPC-BSEL

This directory is a self-contained implementation. It does not link to or
modify the repository's existing FPC or Byte-Select implementation.

For each 64-byte block, FPC-BSEL first classifies sixteen 32-bit words using
the eight FPC patterns. A 16-byte Byte-Select model may compress the expanded
3-bit prefix vector, while a separate 64-byte Byte-Select model may compress a
zero-filled residual block containing only FPC pattern-7 words. Each component
uses BSEL only when its real representation is smaller. The final block is the
smallest real stream among raw, conventional FPC, and FPC+BSEL.

## Build and test

Linux/server:

```bash
cmake -S fpc-bsel -B fpc-bsel/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build fpc-bsel/build -j
ctest --test-dir fpc-bsel/build --output-on-failure
```

Windows PowerShell:

```powershell
cmake -S fpc-bsel -B fpc-bsel/build -G Ninja
cmake --build fpc-bsel/build
ctest --test-dir fpc-bsel/build --output-on-failure
```

## CLI

Inputs must be non-empty multiples of 64 bytes.

```powershell
fpc-bsel train TRAIN.bin model.fpcb --max-patterns 256
fpc-bsel evaluate model.fpcb TEST.bin
fpc-bsel roundtrip model.fpcb TEST.bin
fpc-bsel compress model.fpcb TEST.bin TEST.fpz
fpc-bsel decompress model.fpcb TEST.fpz restored.bin
```

The encoded block mode and FPC+BSEL flags are included in reported sizes.
Container magic, original size, and per-block framing are excluded by
`evaluate`, but are included in the physical file produced by `compress`.

`evaluate` reports two byte counts. `encoded_bytes` follows the repository's
cache-line baseline convention and excludes the external per-block method
selector. `physical_encoded_bytes` includes that selector. Internal
FPC-BSEL prefix/residual flags are included in both counts.

## Spark/Flink large-dataset comparison

The Linux runner trains three Byte-Select paper presets and the standalone
FPC-BSEL model, then evaluates BSEL, FPC-BSEL, and every baseline implemented
by the main project:

```text
fpc, bdi, hybrid, cpack, bpc, huffman
```

Expected input layout (every file must be a non-empty multiple of 64 bytes):

```text
/data/spark-flink/
├── spark-kmeans-large-train.trace
├── spark-kmeans-large-test.trace
├── flink-state-machine-large-train.trace
└── flink-state-machine-large-test.trace
```

Build both executables from the repository root, then run:

```bash
git submodule update --init --recursive
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
cmake -S fpc-bsel -B fpc-bsel/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build fpc-bsel/build -j

bash fpc-bsel/run_spark_flink_all.sh \
  /data/spark-flink \
  results/spark-flink/fpc-bsel-all
```

The output directory contains trained models, raw comparison logs, FPC-BSEL
round-trip logs, and `summary.tsv`. The runner verifies FPC-BSEL round trips
on both test sets before it succeeds. Models, traces, results, and build
directories are intentionally ignored by Git.

For a single dataset:

```bash
fpc-bsel/build/fpc-bsel train train.trace model.fpcb --max-patterns 256
fpc-bsel/build/fpc-bsel evaluate model.fpcb test.trace
fpc-bsel/build/fpc-bsel roundtrip model.fpcb test.trace

build-release/bsel train train.trace bsel.model \
  --block-size 64 --threshold 16 --paper-config bsel-1024-1024-128
build-release/bsel compare bsel.model test.trace \
  --baseline fpc --baseline bdi --baseline hybrid \
  --baseline cpack --baseline bpc --baseline huffman
```

# Byte Select C++ Reproduction

This repository implements the whole-cache-block Byte Select algorithm from
M. Tomei et al., *Byte-Select Compression*, ACM TACO 18(4), 2021. It follows
the generation flow in Section 6:

1. Convert every fixed-size block to its unique simplest pattern and count it.
2. Remove infrequent patterns and patterns whose rank exceeds the dictionary.
3. Remove non-maximal patterns under the paper's pattern partial order.
4. Greedily combine maximal patterns using their least upper bound.
5. Iteratively select the pattern with the greatest uncovered block count.
6. Compress with the first describing pattern and reconstruct bytes by using
   pattern symbols as dictionary selectors.

The 2023 dissertation's graph representation and simulated-annealing search
are a later, broader algorithm family. They are intentionally not mixed into
this reproduction of the paper's pattern-based algorithm.

The default fourth phase uses the dissertation's A*-style lazy exact pruning.
Previously computed coverage values are upper bounds because coverage can only
decrease after selecting a pattern. A priority queue recomputes only candidates
that can still win. `--selection exhaustive` is provided as a reference; both
modes produce the same ordered patterns and marginal counts.

## Build

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure
```

## Train

The input is a raw trace containing adjacent, fixed-size blocks. A final
partial block is ignored during training and evaluation. The paper's
`BSel-256` configuration for 64-byte CPU blocks uses a 32-byte target, one
metadata byte, and therefore a 31-byte dictionary:

```powershell
.\build\bsel.exe train trace.bin bsel256.model `
  --block-size 64 --threshold 1024 --target 32:256:1
```

Use `--selection lazy` (the default) for pruned training, or
`--selection exhaustive` to validate a dataset against full recomputation.
Use `--holdout-middle 10` to reproduce the paper's split: the middle 10% is
excluded from training and evaluated after the model is generated.

The exact named configurations from Section 7.1 are available through
`--paper-config`, which prevents manual metadata/dictionary mismatches:

```powershell
.\build\bsel.exe train trace.bin multi.model `
  --block-size 64 --threshold 1024 --holdout-middle 10 `
  --paper-config bsel-1024-1024-128
```

`bsel-256` is `32:256:1`; `bsel-4096` is `32:4096:2`; and
`bsel-1024-1024-128` is `32:1024:2`, `16:1024:2`, and `8:128:1`.
The final preset exactly models the paper's 30/14/7-byte dictionary budgets:
the metadata includes the size-selection bit and a 15-bit (32/16-byte) or
7-bit (8-byte) pattern index. A preset cannot be mixed with `--target`.

For a small dataset, lower the threshold to `1`. Multiple quantized targets
are trained independently, as specified in the paper:

```powershell
.\build\bsel.exe train trace.bin multi.model `
  --block-size 64 --threshold 1 `
  --target 32:1024:2 --target 16:1024:2 --target 8:128:1
```

`SIZE:PATTERNS:METADATA_BYTES` is explicit because the paper uses a shared
multi-size metadata encoding for `BSel-1024-1024-128`. The paper preset
reserves its high metadata bit as the size-class tag and uses the remaining
low bits as the pattern index; this tag is included in the reported `2/2/1`
metadata-byte budgets. The generated top-level RTL also accepts `target_size_i`
as the allocation or framing signal used to select the physical datapath. That
signal is not a substitute for, or an uncounted alternative to, the metadata
tag.

The paper trains on collections of traces and holds out the middle 10% of
*each trace*. Use `train-list` rather than concatenating files, so a partial
block at the end of one trace cannot shift the next trace's block boundaries:

```powershell
.\build\bsel.exe train-list all.model cpu-a.trace cpu-b.trace gpu.trace `
  --block-size 64 --threshold 1024 --holdout-middle 10 `
  --target 32:256:1
```

The command prints the total and per-trace training/holdout block counts;
its holdout evaluation is the aggregate over the individual middle segments.

Evaluate each workload separately after a shared training run with
`evaluate-list`. It reports every trace, an equal-weight mean of the per-trace
ratios, and a block-weighted aggregate. The former is useful when matching the
paper's workload means; the latter describes aggregate storage or link use:

```powershell
.\build\bsel.exe evaluate-list all.model cpu-a.trace cpu-b.trace gpu.trace
```

For a paper-style mean of means across benchmark sets, retain the set
boundaries with `evaluate-groups`. It computes each group's equal-weight
per-trace mean, then gives every named group equal weight. This matches the
paper's use of CPU, GPU Compute, and GPU Game as separate benchmark sets;
`group_block_weighted` remains available as a traffic-oriented check:

```powershell
.\build\bsel.exe evaluate-groups all.model `
  --group CPU cpu-a.trace cpu-b.trace `
  --group GPU-Compute gpu-compute-a.trace `
  --group GPU-Game gpu-game-a.trace gpu-game-b.trace
```

`analyze-groups` adds the same grouping discipline to the unique-byte upper
bounds used in Figures 4 and 18. Its final
`actual_fraction_of_quantized_upper` compares equally weighted group means,
not the aggregate number of cache lines:

```powershell
.\build\bsel.exe analyze-groups all.model `
  --group CPU cpu-a.trace cpu-b.trace `
  --group GPU-Compute gpu-compute-a.trace `
  --group GPU-Game gpu-game-a.trace gpu-game-b.trace `
  --ideal-metadata-bytes 1
```

Use `analyze-list` when the same aggregation should include the paper's
unique-byte upper bounds. Its `actual_fraction_of_quantized_upper` is a
diagnostic for how much of the model's byte-select headroom is reached; it is
not a comparison against BDI, BPC, or other published codecs.

```powershell
.\build\bsel.exe analyze-list all.model cpu-a.trace cpu-b.trace gpu.trace `
  --ideal-metadata-bytes 1
```

`compare` runs cache-line baselines against a trained Byte-Select model using
the model's same target sizes. It covers the FPC word-prefix format, the
BDI Table-2 single-base modes, the original C-Pack dictionary scheme (Chen et
al., TVLSI 18(8), 2010), Bit-Plane Compression (Kim et al., ISCA 2016), plus
the canonical Huffman format. FPC, BDI, C-Pack, BPC, and Huffman generate real
round-trippable streams; their sizes
come from the actual encoded byte vectors. `hybrid` selects the smaller
FPC/BDI result per block. External raw/compressed selection metadata is not
charged; BDI's internal mode byte is included in its encoded stream.

```powershell
.\build\bsel.exe compare bsel256.model test.trace `
  --baseline fpc --baseline bdi --baseline hybrid `
  --baseline cpack --baseline bpc --baseline huffman
```

For a collection of workloads, use `compare-list`. It prints each trace's
Byte-Select and baseline estimates, followed by equal-weight per-trace means
and a block-weighted aggregate for every selected algorithm. This keeps a
large trace from silently dominating the workload mean while still exposing
the total traffic or storage result:

```text
bsel compare-list bsel256.model cpu-a.trace cpu-b.trace gpu.trace \
  --baseline fpc --baseline bdi --baseline hybrid --baseline cpack --baseline bpc \
  --baseline huffman
```

`compare-groups` accepts the same `--group NAME INPUT [INPUT ...]` syntax and
baseline options. Its `*_mean_of_group_means` rows are the relevant summary
when comparing separately averaged benchmark sets; its `*_group_block_weighted`
rows instead weight every full cache line equally.

## Basic MCC Address Placement

`mcc` models the first memory-controller-side responsibility for a compressed
memory/cache path: assigning each logical subline to a concrete physical
storage address. The intended MCC scenario uses 256-byte sublines, aligns each
new physical placement to 64-byte boundaries, and groups address metadata in
4KiB regions. It reuses the trained Byte-Select model, compresses each full
input block, and places the resulting record in a physical byte address space.
Compressed blocks consume their real encoded byte count (`metadata + dictionary`);
uncompressed fallback blocks consume the original block size. When later records
arrive, MCC placement is versioned. `segment-v1` is the online 64B segment
packing policy: records smaller than 64B are placed into the first partially
used segment that fits. `region-ffd-v2` is the default policy: within each 4KiB
metadata-management window, records are sorted by compressed size and packed
with first-fit decreasing into 64B physical segments. Use `--placement aligned`
to reproduce the older per-record aligned placement. `tail-split-v3` further
splits records larger than 64B into full 64B segments plus a packable tail; this
models an optimistic controller that can place a record's final partial segment
with other tails. `two-ended-tail-v4` keeps v3's `full segments + tail` split,
but uses an online, read/write-oriented policy. Existing compressed data stays
at the low-address end of a 64B segment and one incoming tail may occupy the
high-address end. The controller chooses an older padding segment that leaves
the largest middle gap. For example, 131B is represented as a contiguous 128B
body plus a 3B tail placed at the opposite end of an earlier padding segment.
`spaced-padding-v5` generalizes this without splitting the tail itself. It keeps
all occupied intervals in a segment, reuses sufficiently large middle padding,
and places each new intact tail where its minimum distance from neighboring
records is largest, with at least a one-byte guard.

```powershell
.\build\bsel.exe train test.trace bsel.model --block-size 256 --target 128:256:1
.\build\bsel.exe mcc bsel.model test.trace --base 4096 --limit 16
```

Use `mcc-compare` to run Byte-Select and all software baselines through the
same MCC address-placement model, reporting the quantized ratio before MCC
packing, after `segment-v1`, after `region-ffd-v2`, after `tail-split-v3`, and
after `two-ended-tail-v4`. `spaced-padding-v5` is also emitted as the
hardware-bounded multi-record padding policy:

```powershell
.\build\bsel.exe mcc-compare bsel.model test.trace
```

The summary reports logical input bytes, stored bytes, padding bytes introduced
by 64-byte physical-address segments, physical address-space bytes, the number
of 4KiB metadata regions, and the resulting `quantized_ratio`. This MCC
quantized ratio is the real compressed-memory address-placement ratio:
`original_bytes / physical_bytes`, so alignment zero-fill is charged. Each
`mcc_entry` line maps a `logical_block` to its `physical_address`,
`metadata_region`, stored size, compression flag, selected pattern set, and
encoded metadata. Use `--alignment` or `--metadata-granularity` only when
testing a non-default controller geometry.

The FPC encoder uses packed 3-bit-per-word tags and a zero-payload zero prefix,
so an all-zero 64-byte line occupies 6B as stated in this paper. `fpc-top256`
is a trained FPC residual-dictionary variant used by MCC comparison: it keeps
the 256 most common FPC pattern-7 residual blocks and charges a one-byte index
when a residual block hits, otherwise falling back to the ordinary residual
payload. `fpc-resword256` is a finer-grained variant: it keeps the 256 most
common 32-bit residual words and charges a one-bit hit flag plus an 8-bit index
on hits, or the raw 32-bit word on misses. `fpc-resword64` is the
hardware-lean version with a 64-entry residual-word table, a 6-bit index, and a
256-byte dictionary instead of the 1KiB table needed by the 256-entry variant.
These dictionary stores are treated like the other trained models and are not
charged per trace. The BDI
estimator follows the Table-2 `Base8/4/2-Delta` sizes and tests whether one
base has a feasible signed-delta range. The C-Pack estimator follows the
authors' 16-entry (64B) FIFO dictionary with the `zzzz`/`xxxx`/`mmmm`/`mmxx`/
`zzzx`/`mmmx` patterns, checking the static zero patterns first and emitting
2-8 metadata bits plus 0-4 data bytes per 4-byte word. The BPC estimator
implements the paper's Delta-BitPlane-XOR transform, the original-symbol
encoder for the base word, and the Table-3b plane codes (3-bit single-zero,
7-bit `2..33` zero runs, all-ones, `DBX!=0 & DBP=0`, two-consecutive-ones,
single-one, and 1-bit-flag fallback), with `ceil(log2(symbols))`-bit position
fields that reproduce the published 5-bit fields on the paper's 32-symbol
blocks. These definitions are intentionally documented because alternative
FPC zero-run, multi-base BDI, and dictionary/replacement variants have
different byte counts. Huffman serializes its symbol/code-length table followed
by a canonical Huffman bitstream. Tests decode every retained baseline format
and require byte-for-byte equality.

## Algorithm-Side Reproduction on Representative Traces

The paper's Figure 11/12 evaluate the generated algorithms against FPC, BDI,
C-Pack, and BPC on CPU and GPU memory traces. Those traces come from SPEC,
LULESH, Rodinia, and in-house GPU simulators and are not redistributable, so
this repository reproduces the *algorithm-side* comparison end-to-end on
representative graph-workload data instead: `tools/run_gapbs_demo.sh` dumps the
CSR arrays (vertex offsets + neighbor IDs) of GAP Benchmark Suite graphs as raw
64-byte cache-line traces, trains the paper's three presets on 90% of each
trace, and compares their quantized compression ratios against the five
baseline codecs on the held-out middle 10% (the paper's holdout rule).

```bash
./tools/run_gapbs_demo.sh /tmp/bsel-demo
```

The script builds `tools/gapbs_dump_trace.cc` against the bundled GAPBS
headers, generates `kron16`, `kron18`, and `urand18` graphs (Kronecker and
uniform-random, the Graph500-style generators), splits each array into
`-offsets` (8 int64 values per line) and `-neighbors` (16 int32 values per
line) traces, and reports the following quantized ratios (higher is better):

```text
trace    array      preset                bsel     fpc     bdi  hybrid   cpack     bpc
kron16   offsets    bsel-1024-1024-128   4.165   1.001   3.244   3.244   2.093   1.934
kron16   offsets    bsel-256             2.000   1.001   2.000   2.000   2.000   1.934
kron16   offsets    bsel-4096            2.000   1.001   2.000   2.000   2.000   1.934
kron16   neighbors  bsel-1024-1024-128   1.154   1.000   1.010   1.010   1.000   1.781
kron16   neighbors  bsel-256             1.601   1.000   1.010   1.010   1.000   1.720
kron16   neighbors  bsel-4096            1.154   1.000   1.010   1.010   1.000   1.720
kron18   offsets    bsel-1024-1024-128   4.291   1.002   3.429   3.429   2.148   1.906
kron18   offsets    bsel-256             2.000   1.002   2.000   2.000   2.000   1.906
kron18   offsets    bsel-4096            2.000   1.002   2.000   2.000   2.000   1.906
kron18   neighbors  bsel-1024-1024-128   1.066   1.000   1.001   1.001   1.000   1.660
kron18   neighbors  bsel-256             1.240   1.000   1.001   1.001   1.000   1.654
kron18   neighbors  bsel-4096            1.066   1.000   1.001   1.001   1.000   1.654
urand18  offsets    bsel-1024-1024-128   4.000   1.003   3.910   3.910   2.000   1.844
urand18  offsets    bsel-256             2.000   1.003   2.000   2.000   2.000   1.844
urand18  offsets    bsel-4096            2.000   1.003   2.000   2.000   2.000   1.844
urand18  neighbors  bsel-1024-1024-128   1.000   1.000   1.000   1.000   1.000   1.091
urand18  neighbors  bsel-256             1.000   1.000   1.000   1.000   1.000   1.091
urand18  neighbors  bsel-4096            1.000   1.000   1.000   1.000   1.000   1.091
```

These results reproduce the paper's qualitative findings:

- **Regular integer data (offsets):** `bsel-1024-1024-128` beats the best
  baseline (BDI) by 19% on average (2-28% across the three graphs) and
  crushes C-Pack/FPC/BPC, matching the paper's
  observation that Byte-Select "does much better than CPack at compressing
  traces with mostly integer data" (a 23% average improvement in the 32/16/8
  quantized ratio in the paper).
- **Diverse high-entropy data (neighbors):** BPC wins, exactly as the paper
  reports that "BPC does best for GPU workloads which have more diverse data
  types." `bsel-256`'s single metadata byte keeps it closest to BPC.
- **Random graphs:** everything is near 1.0, matching the paper's note that
  randomly generated input data is largely incompressible.

Honest caveats: these are single-array CSR dumps, not the paper's SPEC/GPU
traces, and the numbers above are *estimates*, not the paper's published
values. The script uses the paper's pattern-count threshold of 16 for the
high-entropy neighbor arrays (its CPU threshold of 1024 prunes everything on
demo-sized traces) and threshold 1 for the offsets arrays; on the small
neighbor traces that threshold understates Byte-Select, and running the full
pattern set on a small graph (`NEIGHBOR_THRESHOLD=1` with a `-g 12` graph)
raises `bsel-1024-1024-128` from about 1.15 to 1.94, still below BPC's 2.78 on
that data. Set `OFFSET_THRESHOLD` / `NEIGHBOR_THRESHOLD` to override.

## Inspect Trained Patterns

The paper's Table 3 reports the top patterns each trained algorithm discovers.
`patterns` prints every pattern set in selection order, with each pattern's
rank, its encoded metadata identifier, and its symbol string. When every
symbol is a single digit, a compact `digits=` form matching Table 3's
presentation is added:

```powershell
.\build\bsel.exe patterns bsel256.model --limit 2
```

The symbol string is the same format stored in the model file: one index per
byte of the block, where equal symbols must hold equal byte values. The first
symbols of a pattern therefore read like Table 3's `11112345` entries.

## Evaluate And Round Trip

```powershell
.\build\bsel.exe evaluate bsel256.model test.bin
.\build\bsel.exe analyze bsel256.model test.bin --ideal-metadata-bytes 1
.\build\bsel.exe compress bsel256.model input.bin output.bsel
.\build\bsel.exe decompress bsel256.model output.bsel restored.bin
```

`evaluate` reports both ratios used in the paper: the unquantized ratio uses
the selected pattern's rank plus its metadata bytes, while the quantized ratio
uses the allocated target size and charges incompressible blocks at the full
block size. It also reports how many blocks use each target.

`analyze` adds the paper's byte-select ideals. `unique_ratio` stores one copy
of every unique byte with free oracle metadata. `unique_with_metadata_ratio`
adds the requested metadata bytes, and `quantized_ratio` rounds each ideal
block to the smallest model target that can hold its unique bytes plus that
metadata. These correspond to the unique-byte, `M1`, and quantized `M1-Q`
measurements in Figures 4 and 18.

The `.bsel` container is a software test format; it includes framing fields
that are not counted by the paper's cache-block metric. Its v2 compressed
records carry the selected target size and the same encoded metadata value
used by the RTL, then derive the pattern index by validating the metadata tag.
It remains able to read the earlier v1 set-index/pattern-index records.

For paper-comparable experiments, reserve the middle 10% of each trace for
testing and train on the remaining 90%. The paper used a frequency threshold
of 1024 for CPU traces and 16 for GPU traces.

## Runtime Cache-Line Traces

`tools/pin/cacheline_trace.cpp` is an Intel Pin tool that models a set-associative
LLC and writes the cache-line contents observed on simulated LLC misses. It
defaults to 64-byte lines for the CPU configuration, and supports the paper's
128-byte GPU lines with `-line-size 128`.
This is the trace semantic used by the paper; it is different from splitting
the benchmark's input file into blocks.

Build the tool from WSL. Pin's compiler wrapper cannot handle spaces in its
root path, so use the Windows short path for this workspace:

```bash
cd /mnt/c/Desktop/BYTESE~1/tools/pin
make TARGET=intel64
```

Example GAPBS BFS capture, restricted to the `DOBFS` kernel:

```bash
cd /mnt/c/Desktop/BYTESE~1
./pin-external-4.3-99850-gce5652921-gcc-linux/pin \
  -t ./tools/pin/obj-intel64/cacheline_trace.so \
  -o ./build/bfs-g16-roi.trace \
  -cache-mb 16 -ways 16 -line-size 64 -roi DOBFS -max-blocks 100000 -- \
  ./gapbs-master/bfs -g 16 -n 1
```

Important tracer options:

- `-cache-mb`: simulated LLC capacity in MiB.
- `-ways`: LLC associativity.
- `-line-size`: cache line size in bytes; it must be a power of two.
- `-roi`: record only misses from the thread currently executing a routine
  containing this text. Accesses outside the ROI still update the simulated
  LLC, so entering an ROI does not reset or freeze cache state.
- `-warmup-misses`: populate the cache and omit the first global simulated LLC
  misses from the trace; this is applied before the ROI output filter.
- `-max-blocks`: bound runtime and trace disk usage.

The tracer is a practical user-space reproduction backend. The paper used
SimNow and an in-house gem5 branch and recorded backing-store contents at the
memory controller. Reproducing the exact published numbers additionally
requires the authors' original traces, simulator changes, workloads, and
commercial 14 nm synthesis library, none of which are distributed with the
paper.

## RTL Generation

Generate hard-coded SystemVerilog compressor and decompressor modules directly
from a trained model:

```powershell
.\build\bsel.exe generate-rtl model.bsel build\rtl
```

One module pair is emitted per quantized target. The compressor hard-codes
the byte equality checks and dictionary selectors for every pattern and picks
the first successful pattern. The decompressor uses the pattern identifier to
hard-code each output byte's dictionary selector. For a tagged multi-size
preset, the pattern identifier field contains the high size-class tag followed
by the low pattern-index bits, matching the metadata layout counted by the
paper.

When Icarus Verilog is installed, CTest additionally compiles and simulates
generated SystemVerilog. This verifies pattern priority, byte dictionaries,
metadata target tags, invalid metadata rejection, and top-level selection of
the smallest successful target. Set `-DBSEL_ENABLE_RTL_SIM=OFF` when a build
environment intentionally has no simulator.

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

cmake -S fpc-bsel.v2 -B fpc-bsel.v2/build -DCMAKE_BUILD_TYPE=Release
cmake --build fpc-bsel.v2/build -j

chmod +x tools/run_final_compression_mcc.sh

./tools/run_final_compression_mcc.sh \
  --input /data/dataset-20g.bin \
  --train-percent 20 \
  --output-dir results/final-20g \
  --chunk-mib 256 \
  --roundtrip \
  --resume
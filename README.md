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

`compare` runs published-format **size estimators** against a trained
Byte-Select model using the model's same target sizes. It currently covers the
FPC word-prefix format and the BDI Table-2 single-base modes; `hybrid` selects
the smaller of those two estimates per block. It does not claim to implement
CPack, BPC, or a complete baseline bitstream/RTL implementation.

```powershell
.\build\bsel.exe compare bsel256.model test.trace `
  --baseline fpc --baseline bdi
```

For a collection of workloads, use `compare-list`. It prints each trace's
Byte-Select and baseline estimates, followed by equal-weight per-trace means
and a block-weighted aggregate for every selected algorithm. This keeps a
large trace from silently dominating the workload mean while still exposing
the total traffic or storage result:

```text
bsel compare-list bsel256.model cpu-a.trace cpu-b.trace gpu.trace \
  --baseline fpc --baseline bdi --baseline hybrid
```

`compare-groups` accepts the same `--group NAME INPUT [INPUT ...]` syntax and
baseline options. Its `*_mean_of_group_means` rows are the relevant summary
when comparing separately averaged benchmark sets; its `*_group_block_weighted`
rows instead weight every full cache line equally.

The FPC estimator uses the 3-bit-per-word header and zero-payload zero prefix,
so an all-zero 64-byte line occupies 6B as stated in this paper. The BDI
estimator follows the Table-2 `Base8/4/2-Delta` sizes and tests whether one
base has a feasible signed-delta range. These definitions are intentionally
documented because alternative FPC zero-run and multi-base BDI variants have
different byte counts.

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

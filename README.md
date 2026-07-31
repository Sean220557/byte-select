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

For a small dataset, lower the threshold to `1`. Multiple quantized targets
are trained independently, as specified in the paper:

```powershell
.\build\bsel.exe train trace.bin multi.model `
  --block-size 64 --threshold 1 `
  --target 32:1024:2 --target 16:1024:2 --target 8:128:1
```

`SIZE:PATTERNS:METADATA_BYTES` is explicit because the paper uses a shared
multi-size metadata encoding for `BSel-1024-1024-128`; this implementation
keeps each independently trained set self-describing.

## Evaluate And Round Trip

```powershell
.\build\bsel.exe evaluate bsel256.model test.bin
.\build\bsel.exe compress bsel256.model input.bin output.bsel
.\build\bsel.exe decompress bsel256.model output.bsel restored.bin
```

`evaluate` reports the quantized compression ratio: original bytes divided by
the allocated target sizes, with incompressible blocks charged at full block
size. The `.bsel` container is a software test format; it includes framing
fields that are not counted by the paper's cache-block metric.

For paper-comparable experiments, reserve the middle 10% of each trace for
testing and train on the remaining 90%. The paper used a frequency threshold
of 1024 for CPU traces and 16 for GPU traces.

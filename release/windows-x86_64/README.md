# Windows x86-64 portable binaries

This directory is ready to copy to another 64-bit Windows machine. Keep the
two DLL files next to the executables. No compilation or MinGW installation is
required.

Executables:

- `bsel.exe`: main Byte-Select program; retained baselines are FPC, BDI,
  Hybrid FPC/BDI, C-Pack, BPC, and Huffman.
- `fpc-bsel.exe`: original standalone FPC-BSEL.
- `fpc-bsel-v1.exe`: optimized v1.
- `fpc-bsel-v2.exe`: optimized v2.

For one `.delete_hole` file, automatically split complete 64-byte blocks into
the first 90% for training and the remaining 10% for testing:

```powershell
.\run-new-dataset.ps1 -InputPath "D:\data\new.delete_hole"
```

To run Byte-Select, every retained baseline, standalone FPC-BSEL original,
v1, and v2 in one pass, use:

```powershell
.\run-all-baselines.ps1 -InputPath "D:\data\new.delete_hole"
```

The unified machine-readable result is written to `output\summary.csv`.
Detailed training, evaluation, comparison, and round-trip logs are written in
the same directory.

Change the split if needed with `-TrainPercent 80`. The generated files are
named `split-train.delete_hole` and `split-test.delete_hole` under `output`.

For two files that are already split, run:

```powershell
.\run-new-dataset.ps1 -TrainPath "D:\data\train.trace" -TestPath "D:\data\test.trace"
```

Input files must be non-empty multiples of 64 bytes. The script trains v2,
evaluates it, verifies a complete block-level round trip, and writes logs under
`output` by default.

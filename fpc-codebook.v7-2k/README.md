# FPC Codebook v7-2K

This version preserves the FPC-BSEL v2 codec and limits the global prefix
codebook to 128 patterns. Each pattern is 16B, so the hardware pattern SRAM
is 2048B. The serialized model adds a 28B file header.

Training uses the 16.77MB Fashion training sample. Independent evaluation uses
the 31.36MB Fashion test set. The measured lossless encoded ratio is 3.5843.
The 4K Top-256 model remains unchanged.

This static Top-N result is the capacity baseline for the next locality-aware
stage: a small global table plus 4KiB-region hot-pattern entries.

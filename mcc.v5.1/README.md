# MCC v5.1: Configurable Guard

MCC v5.1 preserves intact-tail and reusable-middle-padding behavior from v5.
It makes the spacing guard configurable with `--guard-bytes N`. The default is
1B, so existing v5 behavior remains unchanged. A 0B guard maximizes density;
2B and 4B provide stronger separation.

On the 31.36MB Fashion test set with the 3KiB codebook, measured quantized
ratios are 3.156749 (0B), 3.133533 (1B), 3.108940 (2B), and 3.065297 (4B).
This establishes the cost curve for a future hot-write policy: cold data can
use 0B while hot data retains a nonzero guard. The current datasets contain no
write-temperature trace, so v5.1 does not infer synthetic hotness.

# FPC-BSEL 256B Subline Format

This version record standardizes comparison granularity without replacing the
64B FPC lane implementation. One logical compression decision covers a 256B
subline containing four lanes.

- Four 2-bit lane modes share one 8-bit mode map.
- Per-lane mode bytes are not stored.
- Lane payloads are concatenated in lane order.
- Raw fallback is selected once for the 256B subline.
- The charged size is `min(256, 1 + sum(lane payload bytes))`.

The final experiment script uses this format for every FPC-BSEL codebook budget.
All built-in baselines are also invoked with a 256B block size before MCC.

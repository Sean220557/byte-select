# MCC v4: Two-Ended Tail Placement

This directory records the v4 policy without replacing MCC v1, v2, or v3.
The compiled implementation is selected with `MccPlacementMode::TwoEndedTailV4`
or `--placement two-ended-tail-v4`.

The physical granularity remains 64B, logical sublines remain 256B, and address
metadata remains grouped at 4KiB. A compressed size is split into complete 64B
segments and one tail. For example, 131B becomes a contiguous 128B body and a
3B tail.

Placement is online. A segment containing an older compressed tail at its low
end is preferred when the incoming tail fits. The incoming tail is written
backward from the high end, leaving a middle gap between the two records. Each
segment accepts at most one low-end record and one high-end tail. Among eligible
segments, v4 chooses the one that leaves the largest gap.

This policy deliberately trades some of v3's optimistic density for simpler
metadata, fewer co-located writers, and greater separation between independently
read or written fragments. `MccEntry::tail_address`, `tail_offset`, and
`tail_bytes` describe the second fragment.

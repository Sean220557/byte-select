# MCC v5: Spaced Padding Reuse

MCC v5 keeps every compressed tail intact. It does not split a tail into
multiple fragments. Instead, each 64B segment tracks all occupied intervals and
all remaining contiguous padding intervals.

When a new tail arrives, v5 scans older padding intervals that can hold the
whole tail. It chooses the position that maximizes the minimum distance to the
neighboring occupied intervals and reserves at least one byte between unrelated
records. Padding between previously placed records remains reusable, so a
segment may contain more than two records.

Candidate lookup is restricted to the active 4KiB metadata region. This keeps
address metadata local and bounds a direct hardware scan to at most 64 physical
64B segments instead of searching the full memory history.

For example, after placing 20B at the low end and 3B at the high end of a 64B
segment, a later 10B tail is placed near the center of the remaining 41B
padding. The 10B tail remains contiguous.

The implementation is selected with `MccPlacementMode::SpacedPaddingV5` or
`--placement spaced-padding-v5`. MCC v1 through v4 remain available unchanged.

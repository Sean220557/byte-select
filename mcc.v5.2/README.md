# MCC v5.2: Region and Size-Class Index

MCC v5.2 preserves v5.1 placement semantics for `region_lookback=0`. Open
padding is indexed by 4KiB metadata region and five largest-gap classes: 1-4B,
5-8B, 9-16B, 17-32B, and 33-64B. A tail only examines classes large enough to
hold it. Candidate segment IDs are sorted before scoring, preserving the old
tie-break order and therefore the same quantized result.

The implementation reports `candidate_segments_scanned` and
`candidate_gaps_scanned` for ablation. The index bounds hardware lookup to the
selected metadata regions rather than the complete allocation history.

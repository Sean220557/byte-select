# MCC v5.3: Bounded Region Lookback

MCC v5.3 extends the v5.2 index with `--region-lookback N`. Lookback zero only
uses the active 4KiB metadata region. Lookback one also keeps the immediately
previous region's open padding eligible. Tails remain intact and guard spacing
is unchanged.

On the 31.36MB Fashion test set with a 3KiB codebook and 1B guard:

| Lookback | Quantized ratio | Candidate segments | Candidate gaps |
|---:|---:|---:|---:|
| 0 | 3.133533 | 375053 | 938102 |
| 1 | 3.186267 | 1004219 | 2531268 |
| 2 | 3.192328 | 1578990 | 4050023 |

Lookback one recovers most of the available gain. Lookback two adds much more
search and only about 0.19% ratio over lookback one, so one is the recommended
hardware point.

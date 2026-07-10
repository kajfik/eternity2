# SOLVER_EVAL — neural guidance, overlapping decomposition, and color-structure exploitation

Evaluation of three candidate techniques against the existing hunter+LNS
solver (`src/solver.cpp`), 2026-07-05. All experiments on the 5-clue
configuration, fresh starts (no resume), same machine (32 logical cores;
benchmarks used 12 threads for ordering arms, 4 threads for decomposition
arms). Benchmark data and scripts in the session scratchpad; all solver
hooks added behind new flags with defaults unchanged (`--order`,
`--model-file`, `--zone-file`, `--log-placements`, `--mode decomp`,
`--rec-seconds`).

**Metric note.** This solver plays the max-matched-edges game: every run
produces a complete 256-piece board and the score is matched internal edges
out of 480 (records: 461/480 with all 5 clues, 470/480 with 1 clue). The
"pieces placed" framing used elsewhere (e.g. the ~240-piece barrier for
perfect partial placements) maps onto the hunter's DFS depth: pieces on
board = 60 (border ring) + 5 (clues) + depth into the 191 interior non-clue
cells. Both metrics are reported: hunt-mode depth for "pieces placed",
mix-mode score for edges matched.

## Step 1 — codebase

- **Search**: two cooperating move types. *Hunter*: randomized perfect
  border ring, then row-major interior DFS under a mismatch budget (48),
  with exact candidate buckets keyed `[west][north]`, a color-availability
  deficit lower bound, and anytime B&B. *Polisher*: LNS on complete boards —
  exact incumbent-seeded branch&bound re-solve of ≤16-cell regions
  (`RegionSolver`) plus Hungarian optimal reassignment of pairwise
  non-adjacent fault cells. Elite + fresh seed pools connect the two.
- **Representation**: `Board` = per-cell piece index (u8), rotation (u8),
  filled flag; piece sides as color quads (U,R,D,L), grey=0, precomputed for
  all 4 rotations (`QUAD[256][4][4]`).
- **Progress metric**: matched internal edges (`Board::score()`, max 480),
  cross-validated against the e2.bucas.name viewer and `tools/e2lib.py`.
- **Existing ordering heuristic**: candidates within a bucket sorted
  scarce-color-first (`COLOR_RARITY`), ties randomized per restart.
- **Own best results before this evaluation** (from `runs/history_*.log`):
  421/480 (5-clue, from scratch, ~4h wall), 470/480 (1-clue, but seeded from
  the published Joshua_Blackwood_470 board — the solver's own contribution
  there is 0 improvement so far).

## Step 2 — bottleneck profile

2-minute fresh-start runs, 12 threads:

| mode | result |
|---|---|
| mix (default) | 410/480 after 2 min; 16 completions, all early; +197 polish gains over 13k LNS iters, strongly diminishing |
| hunt only | ~250M nodes/s aggregate; avg deepest DFS depth **96–99 / 191** interior cells; max 183; **0 completions in 90 s** despite the 48-mismatch budget |

Conclusions: node throughput is not the constraint (deficit-bound pruning is
cheap and effective); the hunter hits an exponential wall around interior
row 7–8 (~64% board fill), i.e. **piece ordering / branching in the
mid-board is the bottleneck**, and end-game score progress comes almost
entirely from the LNS polisher grinding +1s. This is exactly the seam the
three tracks target.

## Track A — neural-guided search

### A1: heuristic shim

The hunter's per-node candidate ordering is realized as a per-bucket sort at
restart setup (`Buckets::build`): bucket `exact[w][n]` contains precisely
the candidates whose west/north sides are (w,n), so a bucket ordering *is* a
state-conditioned ranking on (candidate quad, matched W/N neighbor colors) —
the E/S neighbors are always empty in the row-major scan. The seam
(`--order default|random|model|zone|model+zone`) z-normalizes each component
(rarity / learned model / zone affinity) across the 764 candidate
piece-rotations and sorts by their sum, at zero per-node runtime cost.
A fully dynamic per-node scorer was rejected deliberately: the hunter
explores ~25M nodes/s/thread, and even a 16-unit MLP per node would cost
2–3 orders of magnitude of throughput — any learned ordering must therefore
be expressible as a function of (candidate, local matched context), which
the bucket sort captures exactly.

### A2/A3: scorer + training data

- Model: `tools/train_scorer.py` — logistic-linear and 16-hidden-unit MLP
  (numpy, manual backprop) over 8 one-hot color slots × 23 colors (candidate
  U,R,D,L + neighbor N,E,S,W; E/S = 0/empty), matching the C++
  `ModelWeights` loader (`--model-file`).
- Data: `--log-placements` records the deepest DFS path of every hunter
  restart (cell, piece, rotation, west/north context) with the restart's
  final depth as outcome. One 4-minute 12-thread run produced **1,349
  restarts / 122,534 placement samples** (11× the 10k target).
- Labels: placements from top-tercile-depth restarts (≥155) positive,
  bottom tercile (≤25) negative, split by restart. **Critically**, each run
  is truncated to its first 25 placements so both classes cover the same
  board positions — without this control the classifier reaches AUC 0.65 by
  learning *where* deep runs go rather than *what* they place (positional
  leakage), and the number is meaningless.

### A4: results

Held-out (by-restart) discrimination:

| model | val AUC |
|---|---|
| logistic-linear | **0.509** |
| MLP (16 hidden) | **0.509** |

There is essentially **no signal** in (candidate colors + matched local
context) that predicts which restarts run deep. Deep vs shallow is decided
by the random ring configuration, the `free_row` draw, and tie-break luck —
not by which exact-match candidate is tried first under this feature set.

Search benchmark (same time budget per arm; hunt = pieces-placed proxy, mix
= edges matched):

Hunt mode: 12 threads × 2 min per arm, ~700 restarts each (so per-run
avgdepth SE ≈ 2, but between-run drift is ±2–3 — hence 4 repetitions for
the decisive pair). Depth = interior cells placed beyond ring+clues (max
191); pieces on board = depth + 65. Mix mode: 12 threads × 2.5 min, final
score (single runs — noisy, ±3–4).

| arm | hunt avgdepth (reps) | mean | hunt maxdepth | mix score |
|---|---|---|---|---|
| default (rarity) | 87, 90, 85, 90 | 88.0 | 188–190 | 411, 414 |
| random | 87 | 87 | 188 | 406 |
| **model (learned)** | **98, 93, 96, 94** | **95.3** | 188–189 | 414, 413 |
| zone (Track C) | 88 | 88 | 188 | 413 |
| model+zone | 97 | 97 | 190 | — |

Three findings:

1. **The existing rarity heuristic is a no-op**: default ≡ random on every
   metric (88 vs 87 avgdepth, identical maxdepth). The ~10% interior color
   scarcity spread, summed symmetrically over all four sides, does not
   move the search. (CLAUDE.md flagged this as "needs a long run to
   confirm" — it can be retired or replaced.)
2. **The learned ordering produces a real, reproducible depth gain**:
   model beats default in **4/4 interleaved repetitions with
   non-overlapping ranges** (93–98 vs 85–90), +7.3 avgdepth ≈ 8% deeper
   average stall point, at zero runtime cost. Mechanism (from the learned
   weights): *consume* scarce colors on the matched W/N sides but avoid
   *demanding* them on the open E/S sides — a directional asymmetry the
   symmetric rarity sum cannot express. This defers color-deficit
   pruning, keeping lineages alive longer.
3. **The depth gain does not detectably convert to mix-mode score** at
   this time scale (411/414 vs 414/413): short-run mix scores are
   polisher-dominated and the noise floor (±3–4) swallows any effect.
   A long paired run is the only way to settle score impact.

Supplementary — classic *perfect placement* framing (mismatch budget
forced to 0, 2 min): best perfect prefix 145 interior cells = **210 pieces
on board**, identical for default (145) and model (144) — at budget 0 the
exact buckets are forced-choice so ordering barely matters; the ~240-piece
regime of published perfect-placement records uses free placement order
and no clue pinning, so the numbers are not directly comparable.

## Track B — decomposition with overlap

### B1–B3: design

- **Scheme**: four 9×9 quadrants with a 2-cell overlap cross (cols/rows
  7–8). Chosen granularity: the existing `RegionSolver`/LNS machinery
  operates on arbitrary cell sets, so quadrant solving = masked LNS; the
  9×9 size keeps each subproblem large enough to contain meaningful faults
  while the 2-cell overlap covers every seam edge with at least one region
  that can optimize it.
- **Subproblem solving** (`--mode decomp`): a full board B0 is built
  greedily (or taken from `--seed-edges`), split into quadrant boards with
  non-quadrant cells unfilled; each quadrant is polished independently in
  parallel by a masked, pool-detached polisher (`Polisher::mask`,
  `local_only`). Piece-pool restriction is automatic: both LNS moves only
  permute pieces already inside the masked cells. Edges to unfilled cells
  are unconstrained (the LNS moves and `RegionSolver` were made
  partial-board-safe).
- **Reconciliation**: merge = unanimous cells first, then best disagreeing
  proposal with an unused piece (local match count), then greedy patch of
  leftover cells with leftover pieces (type-consistent, so always
  completes); then a seam-restricted LNS pass (cells within 1 of the
  overlap cross) for `--rec-seconds`. Conflicts measured: overlap-cell
  disagreements, duplicate-piece evictions, seam-band mismatched edges
  before/after.

### B4: results

All arms 4 threads; decomp and mono started from the *same* greedy board
B0 = 394/480; wall time 3 min 20 s each (decomp: 3 min quadrants + 20 s
reconcile).

| arm | final score | notes |
|---|---|---|
| decomposed + reconcile | **403/480** | quadrant internal scores 133→133, 128→128, 108→117, 104→114 (+18 internal); merge back to 394 before seam polish |
| monolithic polish (same B0) | **413/480** | plain masked-free LNS, same machinery |
| monolithic mix from scratch | 410/480 | reference |

Conflict metrics (decomp arm): 17 of 112 overlap-cell assignments disagreed
between quadrants, 0 duplicate-piece evictions, 4 cells greedy-patched;
seam-band mismatched edges 45 after merge, **36 still unresolved after the
20 s reconciliation pass**. Time split: 180.5 s subproblem solving, 20.7 s
reconciliation, ~0 s init.

The decomposition **lost by 10 edges** to the monolithic solver under an
identical budget, and the loss is structural rather than a tuning artifact:

1. **The overlap does not transmit enough constraint.** Each quadrant
   optimizes 144 internal edges but the 4-quadrant union covers only
   424 of 480 board edges with full freedom; every edge in the overlap
   cross is optimized by two solvers that never see each other's exclusive
   zones, so their locally-optimal overlap fillings diverge (17/112 cells).
2. **Quadrant piece pools freeze the global assignment.** Restricting each
   subproblem to the pieces greedily assigned to it means the decomposition
   can never fix B0's piece-to-region misallocations — exactly the moves
   the monolithic polisher makes freely.
3. **Near-rim quadrants saturate instantly** (133/144 and 128/144 never
   improved in 3 minutes — they were already at their pool's local
   optimum), wasting half the compute budget.

## Track C — algebraic color structure

### C1/C2: the color system is engineered flat

From `tools/color_analysis.py` (piece data only, no solver):

- The 22 colors split exactly into **5 border-frame colors** (1–5: 24 sides
  each, only on edge/corner pieces) and **17 interior colors** (6–22: 48–50
  sides each). The frame colors' 120 sides exactly fill the border ring's
  120 lateral side-slots.
- The **bridge graph** (colors connected if some piece carries them on
  opposite sides) has **exactly 2 connected components**: {1–5} and {6–22},
  with **zero border↔interior bridges**. The border ring is an algebraically
  closed subpuzzle — which `solve_ring` already exploits fully.
- Within the interior component: side counts 48–50 (±2%), distinct bridge
  partners 15–17 of 17 possible, weighted degrees 42–47, clustering
  coefficient **0.969**, density 0.652. **No rare colors, no bottleneck
  colors, no clusters, no natural partitions.** Every color count is even
  (no parity obstructions).

This is the central negative structural finding: Eternity II's interior was
deliberately randomized to be statistically uniform, and 17 mutually
well-connected colors at near-identical frequencies leave nothing for an
algebraic decomposition to grab onto.

### C3: zone map

The only non-uniform zone signal derivable from pure structure is the
**rim-adjacent band**: the inner sides of the 56 edge pieces + 4 corners
have a skewed color distribution (colors 15,19 appear on 6 edge pieces
each; colors 10,11 on 1 each), and the ring-distance-1 cells must match
them. Deeper bands are exchangeable — their predicted distribution is just
the interior side frequency (flat to ±2%).

Zone map (log-affinity vs uniform; bands ≥1 identical by construction):

| rim-affinity (band0 − band1+) | colors |
|---|---|
| strongly rim-leaning (+0.29…+0.39) | 15 (p), 19 (t), 7 (h) |
| mildly rim-leaning (+0.12…+0.15) | 6 (g), 14 (o), 16 (q), 20 (u) |
| neutral (−0.04…−0.02) | 8 (i), 9 (j), 13 (n), 18 (s), 21 (v) |
| interior-leaning (−0.22) | 12 (m), 17 (r), 22 (w) |
| strongly interior-leaning (−0.42) | 10 (k), 11 (l) |

Zone/band layout (ring distance, 16×16; `.` = rim handled by `solve_ring`,
digits = band index used by the banded candidate buckets):

```
. . . . . . . . . . . . . . . .
. 0 0 0 0 0 0 0 0 0 0 0 0 0 0 .
. 0 1 1 1 1 1 1 1 1 1 1 1 1 0 .
. 0 1 2 2 2 2 2 2 2 2 2 2 1 0 .
. 0 1 2 3 3 3 3 3 3 3 3 2 1 0 .
. 0 1 2 3 4 4 4 4 4 4 3 2 1 0 .
. 0 1 2 3 4 5 5 5 5 4 3 2 1 0 .
. 0 1 2 3 4 5 6 6 5 4 3 2 1 0 .
. 0 1 2 3 4 5 6 6 5 4 3 2 1 0 .
. 0 1 2 3 4 5 5 5 5 4 3 2 1 0 .
. 0 1 2 3 4 4 4 4 4 4 3 2 1 0 .
. 0 1 2 3 3 3 3 3 3 3 3 2 1 0 .
. 0 1 2 2 2 2 2 2 2 2 2 2 1 0 .
. 0 1 1 1 1 1 1 1 1 1 1 1 1 0 .
. 0 0 0 0 0 0 0 0 0 0 0 0 0 0 .
. . . . . . . . . . . . . . . .
```

The absolute spread is tiny: the strongest signal in the entire map is a
±0.4 log-affinity nudge on 5 of 17 colors in one 52-cell band. Bands 1–6
carry no information at all (identical distributions by construction —
there is nothing in the piece algebra that distinguishes depth 2 from
depth 7).

Board zones (ring distance): band 0 = the 52-cell ring-distance-1 loop,
bands 1–6 = concentric interior rings. The map was exported as
`--zone-file` weights: candidates whose colors are over-represented on
edge-piece inner sides are preferred in band-0 cells, interior-frequent
colors elsewhere.

### C4: results

See the ordering benchmark table above (arms `zone`, `model+zone`).

## Step 3 — cross-track synthesis

**A × C — zone map as a feature for the scorer.** Tested directly as the
`model+zone` arm: 97 avgdepth ≈ model alone (95–98). Zone adds nothing on
top of the model, consistent with zone alone adding nothing over default
(88 vs 88). The band-0 rim signal is the only zone content, and the model
already implicitly captures the relevant color statistics.

**B × C — decomposition regions from the color zone map.** Void by
evidence: C2 shows the interior color graph is a single dense component
(clustering 0.969, no clusters, counts flat to ±2%), so there exists no
color-derived partition of the board beyond border-vs-interior — and that
one is already fully exploited by the perfect border-ring construction.
There is nothing to draw region boundaries from.

**B × A — neural ordering inside subregions.** Moot twice over: (a) the
decomposition itself lost to the monolithic solver by 10 edges, so
improving its subsolver is optimizing the losing branch; (b) the quadrant
subsolver is the *exact* incumbent-seeded `RegionSolver` B&B — candidate
ordering there only affects node counts before the bound closes, and with
incumbent seeding the initial bound is already optimum−1, so value
ordering has no room to help. The learned ordering is a hunter-specific
lever.

**Surviving combination**: mix mode + model ordering — already benchmarked
(no detectable short-run score effect). The right follow-up is a long
paired run, which per repo policy is user-initiated, plus the no-ML
variant below.

## Step 4 — verdict

### Summary of all benchmarks

| experiment | budget | result |
|---|---|---|
| hunt default / random / zone | 2 min × 12T | avgdepth 88 / 87 / 88 — rarity and zone orderings are no-ops |
| hunt **model** (4 reps, interleaved) | 2 min × 12T | avgdepth **95.3 vs 88.0**, 4/4 wins, non-overlapping ranges |
| hunt model+zone | 2 min × 12T | 97 — no stacking over model |
| mix default / random / model / zone | 2.5 min × 12T | 411,414 / 406 / 414,413 / 413 — within noise |
| decomp vs mono-polish (same B0=394) vs mix | 3m20s × 4T | **403 vs 413** vs 410 — decomposition loses by 10 |
| budget-0 perfect placement | 2 min × 12T | 210 pieces (default) ≈ 209 (model) |
| scorer discrimination (held out) | 122k samples | AUC 0.509 (linear) / 0.509 (MLP) |

### What showed signal and why

**Track A is the only positive result**, and it is a search-shaping
effect, not a prediction effect. The per-placement AUC is chance-level:
you cannot look at one placement and tell a good run from a bad one. But
sorting every candidate bucket by the learned score applies a tiny,
*consistent, directional* bias millions of times per restart, and the
direction it learned is interpretable: offer common colors to your
unfilled neighbors, spend scarce colors on sides that are already matched.
That postpones color-deficit pruning and yields a reproducible ~8% deeper
average stall. Notably, this insight does not need the neural network
anymore — it is expressible as a **hand-coded directional rarity score**
(≈5 lines: `+RARITY[matched sides] − RARITY[open sides]` instead of the
current symmetric sum).

**Track B failed for structural reasons**, not tuning ones: fixed piece
pools freeze B0's piece-to-region misallocation, the overlap cross cannot
transmit enough constraint between solvers that never see each other's
exclusive zones (17/112 overlap cells diverged; 36 seam mismatches
survived reconciliation), and rim-adjacent quadrants saturate instantly,
idling half the budget. The monolithic LNS does strictly more with the
same cycles because its regions are chosen *where the faults are* and its
piece pool is global.

**Track C produced a valuable explanation and a null heuristic.** The
analysis proves the puzzle's interior is engineered flat — 17
near-equifrequent, near-fully-interconnected colors, zero border↔interior
bridges, no parity obstructions — which simultaneously explains why the
zone bias does nothing (there are no zones beyond the rim band), why
decomposition has no natural seams, and why the only exploitable
structure (the closed border subpuzzle) was already exploited. The C2
graph is the *why* behind both other tracks' nulls.

### Recommended next step

For Track A (the winner): ~~implement the directional rarity ordering
natively and verify it reproduces the model arm's gain~~ — **done, see
Addendum**: the hand-coded rule (`--order drarity`) captures only part of
the effect (+1.75 avgdepth, 3/4 wins, not separable from noise), while
the learned weights reconfirmed at +5.5 (8/8 paired wins across
sessions). The learned model encodes per-color structure beyond simple
directional scarcity, so **ship the model**: use
`--order model --model-file eval/model_weights.txt` for long runs, and
run a **long paired mix comparison** (hours, user-initiated) of default
vs model ordering on the 5-clue hunt — the only measurement that can show
whether deeper hunter stalls translate into better completions feeding
the polisher. If it does, retrain the scorer on model-ordered logs (one
iteration of self-improvement) and test on the 1-clue variant.

### Can any of this pass the records (462/480 5-clue, 471/480 1-clue)?

Honest assessment: **not on their own, and probably not combined.** The
current gap is 421 → 462 from scratch, i.e. the solver needs ~41 more
matched edges, while the best technique found here shifts the *hunter's
average stall depth* by 8% and moves 2.5-minute scores by ≲3 edges (below
noise). The end-game is polisher-dominated: progress past ~455 requires
escaping deep local optima on complete boards, and none of the three
tracks touches that regime — neural ordering shapes the constructor, the
color algebra is provably flat, and decomposition subtracts value. The
plausible paths to record territory remain (a) much longer runs seeded
from published near-record boards (the 470 seed for 1-clue is already in
place, needing +1), (b) stronger LNS moves (destroy-and-rebuild,
larger exact regions via better bounds — the CLAUDE.md tuning list), and
(c) porting the region B&B to GPU for orders-of-magnitude more polish
throughput. The 240-piece perfect-placement barrier is likewise out of
reach of these techniques: budget-0 runs stall at ~210 pieces under
row-major order with clues, and ordering tweaks moved that by −1.

### Reproduction

- `tools/color_analysis.py [--zone-out F]` — Track C analysis + zone export
- `solver.exe --log-placements F --mode hunt` — Track A data collection
- `python tools/train_scorer.py LOG --out eval/model_weights.txt` — training
- `solver.exe --order model|zone|model+zone|random --model-file eval/model_weights.txt --zone-file eval/zone_weights.txt` — ordering arms
- `solver.exe --mode decomp --threads 4 --minutes 3 --rec-seconds 20` — Track B (writes `b0_edges.txt`; feed it to `--mode polish --seed-edges` for the paired arm)
- Raw benchmark outputs: `eval/bench_*.txt`; weights: `eval/model_weights.txt`, `eval/zone_weights.txt` (zone file regenerated after a corner-piece counting fix; interior-color ordering verified identical to the benchmarked file)

## Addendum — directional-rarity verification (same day)

The recommended follow-up was executed: `--order drarity` implements the
hand-coded directional rule (`+COLOR_RARITY[matched W,N] −
COLOR_RARITY[open E,S]`, z-normalized, in `Buckets::build`). Verification:
4 interleaved reps × {default, drarity, model}, hunt, 2 min × 12 threads:

| rep | default | drarity | model |
|---|---|---|---|
| 1 | 94 | 96 | 98 |
| 2 | 93 | 98 | 95 |
| 3 | 91 | 97 | 103 |
| 4 | 95 | 89 | 99 |
| mean | 93.3 | 95.0 | **98.8** |

- **Model reconfirmed**: 4/4 wins again → 8/8 paired wins over both
  sessions (sign test p ≈ 0.004), mean gain +5.5 this session. Note the
  whole depth baseline drifted up ~5 between sessions — machine-state
  drift is real, only interleaved comparisons are trustworthy.
- **drarity does not reproduce the full effect**: +1.75 mean, 3 wins /
  1 loss — direction right, magnitude not separable from noise. The
  learned weights carry per-color information beyond scarcity (e.g. they
  penalize *offering* colors 6/8 far more than rarity implies and are
  indifferent to 11, the most common interior color — plausibly network
  effects the flat scarcity numbers can't see). Conclusion: keep
  `--order model` (the weight file is 10 KB and costs nothing at
  runtime); `drarity` stays available as the no-file approximation.

**Windfall**: rep 2's *default* arm completed a board at **433/480 — a
new project best for the 5-clue variant** (+12 over the previous 421),
from a stock 2-minute hunt. Independently validated (piece multiset,
clues, grey border; `e2lib.score_edges` = 433). Preserved in
`eval/best_c5_433.txt`, promoted to `runs/best_c5.txt` (old 421 kept at
`eval/best_c5_421_previous.txt`), history line appended. Two lessons:
(a) the hunter *can* complete boards from scratch at budget 48 — rarely,
and the resulting scores can beat hours of resumed polishing; (b) the
421 plateau was likely a polisher local-optimum artifact, so
fresh-lineage hunting deserves more of the mix budget than assumed.

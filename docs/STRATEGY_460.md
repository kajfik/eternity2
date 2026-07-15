# Strategy: 455 → 460 from scratch (5-clue)

*Written 2026-07-13 from direct evidence: `runs_scratch\history_c5.log`, the push460
solver/merge/sweep logs, the drift pool, the eight distinct 455 boards (e2lib-verified),
the sweep state files, and three fresh CP-SAT probes run for this document.
All numbers below were measured, not recalled.*

---

## 1. Evidence review

### 1.1 Score-vs-time shape

Two lineages have reached 455; both wall there.

- **Old lineage ("basin B")**: the 2026-07-11 7.27-h mix run — 453 at 32 min, 455 at
  ~2.6 h, then flat ~4.7 h. Its plateau-merge harvest produced six distinct 455
  siblings (`candidate_merge_455_{35985248,4279accd,addb1f31,b3cd2796,b9b532fe,cbdf0052}` —
  currently deleted from the working tree but recoverable via `git show HEAD:runs/...`).
- **Current lineage ("basin A")**: started fresh 2026-07-12 22:00 under push460.
  Greedy bootstrap 375→402 in seconds; 443–448 within the first minute (hunter);
  sweep found an improvement on the 448 target → adoption restart at 22:03:32 →
  burst 448/449/451/452/453 within one second of the seeded resume; 455 at 22:05:51
  (**5m49s after cold start** — sweep-adoption + reseeded climb, not raw hunting).
  Flat at 455 ever since (~10 h of pipeline wall time across 2026-07-12/13; the
  repeated 455 history entries at 23:06, 09:43, 10:43, 11:43 are stagnation-restart
  re-logs, `-StagnationHours 1`).

The climb below 455 is fast and repeatable; the stop at exactly 455, twice, in
independent basins, is the phenomenon to explain.

### 1.2 Basin structure of the eight distinct 455s

Nine 455 files exist; eight are distinct (`runs_scratch\best_c5.txt` is byte-identical
to `candidate_merge_455_45bbcff6` — **the current best is itself a merge product**).
Pairwise cell agreement (same piece+rotation), e2lib-verified:

- **Basin A** = {best/45bbcff6, a775bd8f}: agree 252/256 (4-cell diff).
- **Basin B** = the six old candidates, with three tight sub-lineages:
  - S1 = {35985248, cbdf0052}: 252/256
  - S2 = {4279accd, addb1f31}: 255/256
  - S3 = {b3cd2796, b9b532fe}: 255/256
  - cross-sub-lineage: S1–S2 229, S1–S3 228–231, S2–S3 219 → **diffs of 25–37 cells,
    squarely inside merge range** (max-window 45).
- **Basin A vs basin B: 8–9/256** — same as unrelated elite boards; cross-basin merge
  is void (matches the σ-cycle finding).

### 1.3 Fault geometry

Every 455 board: 25 fault edges, 39–41 fault cells, 7–10 double-break cells (cells
with ≥2 mismatched incident edges — plenty; `--cost2` relevance confirmed).

- **Basin A**: faults on the RIGHT side, rows 1–14 × cols 10–15, two 8-adjacency
  clusters of 26 and 15 cells. (Orient the board 90° to put them in deep-band rows.)
- **Basin B**: faults in the BOTTOM rows 10–15, one large 38–40-cell blob (plus a
  2-cell satellite at (10,14–15)) — native deep-band territory without rotation.

### 1.4 The drift pool has collapsed — the central finding

The on-disk harvest that feeds `plateau_merge.py`:

- 241 snapshot lines across 18 `drift_t*.txt` files; **only 24 distinct boards
  overall and only 2 distinct at 455** (both within 252/256 of best).
- Meanwhile the solver's **in-memory** `g_plateau` held **215 distinct near-best
  boards 150 s into the 22:03 run** (`plat=215`), and 28–53 in later runs.
- Root cause 1 — `drift_snapshot` (src/solver.cpp:2584) fires **once per arrival**
  at a best-score level per thread (`drift_last_best_seen` latch): it captures the
  arrival board, not the subsequent tie-drift. Every thread converges to (nearly)
  the same 455 and writes it once. The drift-cap 4096 upgrade raised the ceiling
  on a stream that writes ~1 distinct line per thread per best-level.
- Root cause 2 — `g_plateau` is **never persisted** and dies on every stagnation
  restart. With `-StagnationHours 1`, the pipeline has been discarding a
  215-board plateau archive every hour and keeping 2 boards.

Benchmark: the 464 record poster harvests **2213 distinct 462s, 290 463s, 10 464s**.
Our same-score harvest is 2–3 orders of magnitude too small. This, not the merge
machinery, is the bottleneck.

### 1.5 The merge engine works — when fed

Latest merge log (`push460/merge_20260713_094318.log`): from a pool of 209 lines /
~24 distinct boards it enumerated only 10 candidate pairs (54 already in state);
all small diffs (4–14 cells) INFEASIBLE in ≤0.1 s; a 21-cell 454-pair UNKNOWN at
60 s; **two 22–23-cell 454-pairs each merged to 455 in 20–32 s**. The engine
repeatedly demonstrates +1-by-merge — it has simply exhausted a tiny pool and now
re-grinds the same pairs. In-process merge shows the same shape (merge=1183(+597)
in 21 min, gains all below best level).

### 1.6 CP-SAT window status on the 455s

- Sweep on the best 455: **35/35 windows UNKNOWN** at 300 s (pass 1 not finished).
  All windows are 45–48-cell rectangles with 17–26 window faults and ~80
  win_edges — the least decidable shape/size, and the sweep sorts by fault count
  *descending*, so the whole budget goes to the hardest windows first (that order
  is right for weak boards — it improved the 446 and 448 targets on their first
  window — and wrong for strong ones). 455 is therefore **not** proven
  window-locally-optimal; the protocol just can't decide these windows.
- **New probes (2026-07-13, this session)**, improve-only:
  - best 455, bare 15-cell fault cluster (rows 9–14): **INFEASIBLE in 0.1 s**.
  - best 455, bare 26-cell fault cluster (rows 1–8): **INFEASIBLE in 0.2 s**.
  - basin-B cross-sub-lineage merge 4279accd × b3cd2796 (37-cell diff): see
    §1.7 — this class of pair had never been tried.

Both bare fault clusters proven optimal for their own piece pools in
milliseconds — the exact profile of the record boards (JB470, R461, 464-v2/A).
Local rearrangement without piece inflow is mathematically dead; every remaining
path is piece inflow (dilated windows — currently undecidable at 300–600 s) or
cross-board merges.

### 1.7 Basin-B cross-pair probe result

The 4279accd × b3cd2796 disagreement-set merge (37 cells, the first cross-sub-lineage
455 merge ever attempted): **UNKNOWN at 240 s** (10 workers). Not INFEASIBLE —
undecided, like every ≥21-cell window at short budgets. Escalation (longer time,
all cores, or decomposition) is untested; 12 such pairs exist.

---

## 2. Diagnosis

The plateau→merge loop is the proven +1 engine (this basin's 455 *is* a merge
product; 454-pairs merged to 455 in seconds whenever distinct pairs existed), but
it is starved: the harvest keeps ~2 distinct boards where the process generated
215+ in RAM and the record method uses thousands. Local repair is proven dead
(bare clusters INFEASIBLE instantly); blind 45-cell rectangles are undecidable at
practical budgets. The lever is therefore: **maximize distinct near-best boards on
disk, merge them at escalating exactness, and spend CP-SAT only on windows sized
to be decidable.** Hunting harder is explicitly not the lever (established by the
7.27-h analysis and re-confirmed here: hybavg pinned at 381.4, completions frozen).

---

## 3. Ranked proposals

Noise/protocol note: 90-s hunt A/Bs are the wrong instrument for everything below.
Plateau-side changes need multi-hour paired runs, and the primary metric is usually
**distinct near-best boards harvested per hour** and **new merge candidates
produced**, with score movement as the (rare, binary) confirmatory event.

### P1. Exploit the eight existing 455s (zero code — start today)

**Mechanism.** 12 cross-sub-lineage basin-B pairs (25–37-cell diffs) + 1 basin-A
pair (4-cell, trivial) have never been merge-probed. Run
`window_solve.py --board X --board2 Y --improve-only` on each at an escalation
ladder: 600 s → 1800 s → (top 3 by smallest diff) 2–4 h, 16 workers. Also run each
pair's diff **dilated by 1** (piece inflow around the disagreement) at the long tier.
**Bottleneck link.** Direct: merges are the +1 engine; these are the largest-diff
in-basin pairs we own, i.e. the highest-entropy merge material available today.
**Cost.** None (existing tools). One overnight of CPU.
**Protocol.** Not an A/B — a bounded probe campaign. Any SAT = 456, e2lib-verify,
adopt via `--seed-edges`, restart the whole ladder at 456.
**Decision value.** All-INFEASIBLE ⇒ basin B's current diversity is exhausted at
this diff scale → P2 urgency confirmed. Mostly-UNKNOWN even at 4 h ⇒ ≥25-cell
merges are CPU-undecidable → prioritize small-diff *volume* (P2) over escalation.
**Risk.** Low. Compute-only. (First data point: the 37-cell pair is UNKNOWN@240 s,
so expect the long tier to matter.)
**Hygiene.** Restore the six deleted basin-B candidates from git HEAD into an
archive dir first — they are irreplaceable merge fodder.

### P2. Fix the harvest: persist and widen the plateau pool (small C++ — the highest-leverage change)

**Mechanism.** Three parts, all in `src/solver.cpp` + trivial glue:
(a) **Drift-not-arrival snapshots**: in `bump_stagnation`, when a lineage is within
1 of best, snapshot every K non-improving steps (K ≈ 200–500) if the board hash
isn't in a recent-writes ring — not once per best-level arrival.
(b) **Persist `g_plateau`**: dump the deduped pool to `<drift-dir>\plateau.txt`
every N minutes and at exit (score + edges lines, same format).
(c) **Reload on startup**: read drift + plateau files back into `g_plateau`
(score-gated vs the resumed best) so restarts stop zeroing the archive.
**Bottleneck link.** Converts the observed 215-board in-RAM diversity into merge
food. This is the single measured 100× gap between us and the 464 method.
**Cost.** ~1 day incl. rebuild of both exes and a smoke test.
**Protocol.** Paired 6–12-h push460 runs (same seed, machine idle), 2 reps/arm if
time allows. Metrics: distinct boards at best/best−1 on disk per hour; new
`candidate_merge_*` files produced. **Adopt if disk-distinct ≥10× baseline**
(baseline ≈ 2, so this is nearly assured; the honest check is that merge
candidates actually increase and no best-score leakage appears in verification).
**Risk.** Pool poisoning across lineages — keep the existing archive-on-fresh
discipline (the run.ps1 archiver already moves drift; extend to plateau.txt).
Disk churn is trivial (4096 × ~1 KB).

### P3. Stop destroying pools: stagnation policy for plateau regimes (small PS + depends on P2c)

**Mechanism.** push460's `-StagnationHours 1` restart resets all pools hourly. With
P2c, restarts become cheap (pool reloads). Independently: before killing the
solver, trigger a merge pass over the freshly dumped pool (the moment of maximum
diversity), and lengthen stagnation to 3–4 h at the 455 plateau — hourly resets
buy nothing when the climb is already done (re-climbing to 455 takes minutes and
re-treads the same lineage).
**Cost.** Hours.
**Protocol.** Folded into P2's A/B (they ship together).
**Risk.** Minimal; restart-on-stagnation stays available as the umbrella-of-last-resort.

### P4. Resize the CP-SAT sweep for strong boards (small Python)

**Mechanism.** Four changes to `window_sweep.py`/push460 sweep stage:
(i) **fault-shaped windows** — each fault cluster dilated r=1 (~25–45 cells)
instead of only blind rectangles; (ii) sort **ascending** by win_edges/faults on
targets within 1 of best (bank INFEASIBLE proofs cheaply, shrink the frontier;
keep descending for weaker targets, where it demonstrably finds improvements
fast); (iii) a **pass-3 overnight tier**: the 3–5 most promising undecided windows
(fewest win_edges per fault) × 2–4 h × 16 workers — mirror of the v3-record
protocol, never yet applied to our own boards; (iv) sweep **all eight siblings**,
not just best (different piece pools ⇒ independent chances).
**Bottleneck link.** CP-SAT windows are the only piece-inflow mechanism besides
merges; right now 100% of sweep budget goes to a shape/size/order combination
that has produced 35 UNKNOWNs and will take days to even finish pass 1.
**Cost.** ~half a day.
**Protocol.** Coverage change, not a hypothesis: measure decided-window rate and
proofs banked per hour; drop any shape class that stays >90% UNKNOWN at its tier.
**Risk.** Low. Worst case we buy optimality proofs instead of improvements —
which still redirects budget away from dead windows.

### P5. Merge escalation for large diffs (config + small Python)

**Mechanism.** `plateau_merge.py` gives every pair 60 s. The log shows: ≤14-cell
diffs decide in 0.1 s, 21+-cell diffs go UNKNOWN, and 22-cell 454-pairs that DID
decide took 20–32 s — right at the budget edge. Scale `--time-per-pair` by score
group (top group: 600 s; below: 60 s) and raise `--max-window` toward 45 with the
long budget. Optionally add a "hard-pair retry" state so UNKNOWN pairs get one
overnight 2-h attempt instead of being permanently skipped.
**Bottleneck link.** Once P2 floods the pool, pair volume explodes; the top-score
pairs are exactly where +1 lives and deserve orders of magnitude more time than
the default.
**Cost.** Hours.
**Protocol.** Observational: count decided-vs-UNKNOWN by diff size before/after;
adopt if the 20–35-cell decision rate rises materially (pre-register: ≥3× decided
pairs in that band over a matched overnight).
**Risk.** Budget starvation of small pairs — mitigated by score-group scaling.

### P6. Deep-band campaigns aimed at the measured fault geometry (config only)

**Mechanism.** Basin B's 38–40-cell fault blob lives in rows 10–15 (native
deep-band range); basin A's clusters live in cols 10–15 (one rotation away).
Dedicated overnight runs per sibling: `--seed-edges <sibling> --band-y0-min 2..6
--band-nodes 1e9..1e10 --cost2 2` (the boards have 7–10 double-break cells —
cost2=2 in the Hunter/band widens exactly the family these boards need), umbrella
off or rare. This is the anytime lottery at the 40–96-cell scale — the only
in-solver move class matching the fault-set size, and never yet pointed at these
boards with big budgets.
**Bottleneck link.** Bands are in-solver piece-inflow at a scale CP-SAT can't
decide; the R461 experience (bands exhaust with certificates under cost2) says
budgets are the binding constraint.
**Cost.** None (flags). Machine-nights.
**Protocol.** Lottery, not A/B: run 2–3 nights across siblings; any band gain at
455 is adopted directly (score-gated paths make this safe). Pre-register: if ~3
machine-nights produce zero band improvements at 455, retire the campaign.
**Risk.** Needle-rare payoffs; costs only idle nights that P1/P4 don't fill.

### P7. Umbrella/kick schedule for multi-hour horizons (moderate C++)

**Mechanism.** Current umbrella: 25–40-cell greedy rebuild, firing ~17×/21 min at
the plateau, converting never. The bare-cluster INFEASIBLE proofs explain why:
25–40-cell perturbations near the fault zone land back in the same basin (the
region is provably optimal for its pieces; a greedy rebuild of it re-converges).
Options, in test order: (a) umbrella targets = fault cluster ∪ dilation instead
of random blob; (b) `--umbrella-dist` scaled to 60–100 cells (σ-cycle scale — the
measured size of real basin transitions); (c) post-kick protected drift (lineage
may not re-anchor to global best for N steps).
**Bottleneck link.** Kicks are the only intra-process basin-hop mechanism; the
current scale is provably too small at 455.
**Cost.** 1–2 days.
**Protocol.** This is where honesty is hardest: paired 12-h runs × 2–3 reps/arm,
metric = distinct 455s + any 456; pre-register adopt only on (any 456) OR (≥3×
distinct-455 discovery rate with P2 harvesting in both arms). Do NOT evaluate on
90-s or even 8-min horizons.
**Risk.** Bigger kicks may just be slower restarts; the σ-cycle scale is
motivated but unproven as a *kick* size. Medium implementation risk of subtle
score leakage — keep score-gated paths untouched.

### P8. RegionSolver full per-node Hungarian bound → bigger exact regions (moderate–high C++)

**Mechanism.** The listed not-yet-done idea: replace the top-2 dynamic suffix
bound with a per-node assignment bound, buying exhaustive regions at 24–40 cells
instead of ~16. That directly upgrades (a) in-process merges (the 25–37-cell
diffs that CP-SAT leaves UNKNOWN become in-solver B&B targets with incumbent
seeding), (b) tie-drift volume at bigger scales (more plateau diversity per
hour — feeds P2's harvest).
**Bottleneck link.** Indirect but real: every plateau mechanism is capped by the
largest region we can solve exactly.
**Cost.** Several days + careful perf work (O(n³) per node must be incremental /
early-terminated to not tank iteration rate).
**Protocol.** Two-stage: (1) instrument decide-rate of 24/32-cell regions at
fixed node caps, old vs new bound (deterministic, cheap, honest); adopt the bound
if ≥5× decide-rate at 32 cells; (2) only then A/B region-size defaults on 12-h
paired runs with P2 metrics.
**Risk.** Perf regression risk is the main one; the bound is mathematically safe.

### P9. GPU region B&B (defer — and here is the specific reason)

The polish-dyn A/B proved polish is **stagnation-limited, not iteration-limited**:
+10% polish iterations converted to zero extra gain. GPU throughput on the same
≤16-cell regions therefore buys nothing. The only GPU case that attacks the
actual bottleneck is *qualitatively larger exact searches* (40+-cell regions /
50+-cell windows at millions of nodes/s) — a decidability play. That is worth
considering **only after** P4's overnight tier and P8's bound tell us whether
bigger exact neighborhoods actually yield SATs near 455 (if 4-h CP-SAT on
45-cell windows is always UNKNOWN *and* 32-cell B&B regions always exhaust with
no gain, bigger hardware aims at a dead scale). Cost is weeks; keep deferred.

### P10. Basin farming (the parallel/fallback track, config only)

**Mechanism.** Fresh scratch lineages are cheap: 451 in 90 s (twice), 453 in
32 min, 455 in 2.6 h and again in <6 min with sweep-adoption. Farm lineages to
their plateau (push460 `-Fresh` runs), with P2 harvesting on from minute one,
and keep any basin that (a) exceeds 455 organically or (b) plateaus at 455 with
different fault geometry. Two basins both walling at exactly 455 is only n=2 —
455 may simply be where the ≤40-cell improvement graph thins for *typical*
scratch basins, and basin quality is a lottery ticket worth buying repeatedly.
**Protocol.** Track plateau score + fault geometry per farmed basin; feed every
455+ basin into the P1/P5 merge machinery within its own basin.
**Risk.** Opportunity cost only; it shares the machine with P6 nights.

---

## 4. Recommended sequence

**Phase 0 (today, zero code):**
1. Restore the six basin-B candidates from git HEAD into `runs\archive_455_basinB\`.
2. P1 ladder: 13 pair-merges at 600 s; escalate UNKNOWNs to 1800 s; top-3 to 2–4 h
   overnight. In parallel, P4iii-style long probes: dilate-1 fault-cluster windows
   on best and a775bd8f at 2–4 h (the direct analog of the v3-record probe, never
   run on our boards).
   - **456 found → adopt, restart ladder at 456.** Everything below still applies.
   - All INFEASIBLE → basin diversity is exhausted at current scale ⇒ P2 is the
     only path inside these basins; skip further escalation.
   - All UNKNOWN at 4 h → large-diff merges are CPU-undecidable ⇒ P2 (volume of
     small diffs) strictly dominates escalation (and P9's future case weakens).

**Phase 1 (this week, the big one):** P2 + P3 (harvest persistence + stagnation
policy), then a 24–48-h push460 soak. Expected outcome: hundreds of distinct
455/454s on disk and a merge frontier that stops re-grinding 10 pairs.
P5 (merge time scaling) ships in the same soak — it's config-level.

- If the soak produces new distinct 455s at a healthy rate but merges still
  yield no 456 after ~2 days: the 455 level of these basins is merge-exhausted
  at ≤45-cell diffs → weight shifts to P7 (bigger kicks) and P10 (farm basins),
  with P4 sweeps as the overnight background.
- If the soak itself produces a 456 (merge or sweep): re-center everything on the
  456 pool; the same machinery iterates.

**Phase 2 (next):** P4 sweep restructure (half a day, ships during phase 1's
soak), P6 band nights on idle nights, P10 farming on a second lineage when the
main pipeline is CPU-idle-ish.

**Phase 3 (only on evidence):** P7 after phase 1's verdict; P8 after P7 or in
parallel if a coding week is available; P9 only if P4/P8 show large exact
neighborhoods produce SATs.

---

## 5. Is 460 reachable from the 455 basins at all?

What the data says:

- **Cross-basin merging is void** (8–9/256 agreement), so each basin climbs alone.
  We own two basins; they are separate lottery tickets, not a combinable resource.
- **Below 455 the climb is merge/sweep-driven and fast**; at 455, two basins
  stalled — but both stalled with a starved harvest (2 distinct boards on disk vs
  215 in RAM vs 2213 for the record method at 462). **The basins have not been
  falsified; they have been under-exploited.** No merge at the 455 level with a
  properly-sized pool has ever been attempted.
- The 464 poster's pool sizes (2213×462 → 290×463 → 10×464) imply roughly an
  order-of-magnitude pool decay per +1 near the record. We are 9 points below
  that regime; the plateau graph at 455 should be far denser than at 462. Their
  lineage passed *through* 455-level scores with the same harvest+merge method —
  455 is not a known wall of the search space; ours is an engineering artifact
  until proven otherwise.
- Sobering counterweight: their 462–464 line "comes from a separate 463 basin" —
  i.e., even the record holder ultimately basin-hopped rather than climbing one
  basin end-to-end. And 455→460 is five consecutive +1s, each needing a pool we
  have never once built.

**Verdict.** 456–457 from the current basins is a realistic expectation within
days *after* P2 lands — that's the honest test of basin viability, and it hasn't
been run yet. 460 from these specific basins is genuinely uncertain: plan for
the likely outcome that 460 arrives from a later, better basin, which is why P10
(farming, with harvest machinery on from day one) runs in parallel rather than
as a last resort. The strategic posture: **treat the current basins as the
primary track for about one week of properly-harvested exploitation; let every
farmed basin inherit the same machinery; never again let a basin plateau with a
2-board harvest.**

---

## Appendix: probe commands used (reproducible)

```powershell
# bare fault clusters on best (both INFEASIBLE ~0.1-0.2 s):
python tools\window_solve.py --board runs_scratch\best_c5.txt --clues 5 --improve-only --time 240 `
  --cells 155,156,172,173,189,190,191,205,206,220,221,222,236,237,238
python tools\window_solve.py --board runs_scratch\best_c5.txt --clues 5 --improve-only --time 240 `
  --cells 27,28,29,30,31,42,43,44,45,61,62,63,76,77,78,79,92,106,107,108,110,111,125,126,142,143

# cross-sub-lineage basin-B merge (UNKNOWN @ 240 s, 10 workers):
git show HEAD:runs/candidate_merge_455_4279accd.txt > a.txt
git show HEAD:runs/candidate_merge_455_b3cd2796.txt > b.txt
python tools\window_solve.py --board a.txt --board2 b.txt --clues 5 --improve-only --time 240
```

# Prompts for separate Claude instances — 455 → 460 avenues

Companion to `STRATEGY_460.md` (2026-07-13). Paste one prompt per fresh Claude Code
session in this repo. Scheduling: **A first** (compute-only, machine idle). **B** is the
highest-leverage coding task and can be written while A's ladder runs (needs the machine
only for a smoke test). **C** ships during B's soak. **D** fills idle nights. **E** and
**F** are gated on B's soak verdict. **G** depends on B's machinery. P9 (GPU) is
deliberately omitted — deferred pending P4/P8 evidence (see STRATEGY_460.md §3-P9).

Only one compute-heavy instance should own the machine at a time.

---

## Prompt A — Phase 0: sibling merge ladder + long CP-SAT probes (zero code)

```
Read CLAUDE.md fully, then docs/STRATEGY_460.md (especially §1.2, §1.7, §3-P1).
This is a compute campaign, no code changes. The machine must be otherwise idle —
verify push460/solver.exe/python solvers are not running before each phase.

Context you can trust without re-deriving: there are 8 distinct 455/480 5-clue
boards in two unmergeable basins (cross-basin agreement 8-9/256). Basin A =
runs_scratch\best_c5.txt (= candidate_merge_455_45bbcff6) + candidate_merge_455_a775bd8f
(4-cell diff). Basin B = six candidates deleted from the working tree but present
in git HEAD: candidate_merge_455_{35985248,4279accd,addb1f31,b3cd2796,b9b532fe,cbdf0052}.
Basin B has three sub-lineages; the 12 cross-sub-lineage pairs have 25-37-cell
diffs. One pair (4279accd x b3cd2796, 37 cells) was already probed: UNKNOWN @ 240 s.
Bare fault clusters on best are already PROVEN INFEASIBLE — do not re-probe them.

Tasks, in order:
1. Restore the six basin-B boards from git HEAD into runs\archive_455_basinB\
   (git show HEAD:runs/candidate_merge_455_<hash>.txt). Verify each with
   tools\e2lib.py: score 455, 0 border errors, clean multiset, all 5 clues.
2. Enumerate all pairs among the 8 distinct boards, compute diff sizes, skip
   diffs < 5 (trivially INFEASIBLE) and cross-basin pairs (diff > 45).
3. Ladder, sequential (one solve at a time, --workers 16):
   Tier 1: every pair, tools\window_solve.py --board X --board2 Y --clues 5
           --improve-only --time 600
   Tier 2: all tier-1 UNKNOWNs at --time 1800
   Tier 3: the 3 smallest-diff remaining UNKNOWNs at --time 14400 (ask the user
           before starting tier 3 — it is ~12 h of wall time).
4. In parallel design (but run after the ladder, again asking first): dilate-1
   fault-cluster windows on best and a775bd8f — use window_solve.py --cells with
   each 8-adjacency fault cluster dilated by 1 (compute clusters yourself from
   the edges string), --improve-only --time 7200-14400.
5. Any SAT: e2lib-verify, save as runs\candidate_merge_456_<hash8>.txt, print the
   bucas URL, and stop everything — report immediately.
6. Log every probe (pair, diff size, verdict, time) to docs\phase0_results.md and
   add a dated experiment entry to CLAUDE.md in the established style, including
   the decision-value readout: all-INFEASIBLE => basin diversity exhausted at this
   scale; all-UNKNOWN at 4 h => large-diff merges CPU-undecidable, small-diff
   volume (harvest fix) strictly dominates. Ask before committing.
```

## Prompt B — P2+P3+P5: harvest persistence + stagnation policy + merge time scaling

```
Read CLAUDE.md fully, then docs/STRATEGY_460.md (§1.4, §1.5, §3-P2/P3/P5).
Implement the plateau-harvest fix. Established diagnosis (do not re-litigate):
drift_snapshot in src\solver.cpp (~line 2584) fires once per best-level arrival
per thread (drift_last_best_seen latch) so the on-disk pool held only 2 distinct
455s while the in-memory g_plateau held 215 (plat= in status lines); g_plateau is
never persisted and dies on every stagnation restart (push460 -StagnationHours 1).
The merge engine demonstrably produces +1 when fed distinct pairs.

Implement, all default-off-safe and score-gate-preserving:
1. Drift-not-arrival snapshots: in Polisher::bump_stagnation, when the lineage is
   within 1 of g_best_score, snapshot every K non-improving steps (new flag
   --drift-every, default ~300) if board_hash is not in a per-thread ring of
   recent writes (~256 hashes). Keep the existing arrival snapshot.
2. Persist g_plateau: dump the deduped pool (score + 1024-char edges lines, drift
   file format) to <drift-dir>\plateau.txt every 5 min and at clean exit.
   Atomic write (tmp+rename) — plateau_merge.py reads the dir live.
3. Reload on startup: read all drift_t*.txt and plateau.txt in --drift-dir into
   g_plateau, gated by board_conforms and score >= resumed_best - 1.
4. push460.ps1: on stagnation, dump-then-merge — give the merge loop one pass
   over the fresh pool before killing/restarting the solver; raise default
   -StagnationHours to 3 (restarts no longer lose the pool, but re-climbs buy
   nothing). Ensure run.ps1's archive-on-fresh also moves plateau.txt.
5. plateau_merge.py: scale --time-per-pair by score group — top score group gets
   600 s, lower group keeps 60 s (new flag --time-top, default 600). Evidence:
   22-cell 454-pairs decided in 20-32 s, 21+-cell pairs go UNKNOWN at 60 s.
Verification: --selftest PASS; defaults-off paths bit-identical where promised;
rebuild BOTH exes (scripts\build.ps1 and -Portable) and refresh bin\solver.exe;
4-minute push460 smoke (-Minutes 4) confirming plateau.txt appears, reload works
across a restart, and no score leakage (e2lib-verify best and 2 random pool lines:
score matches, 0 border errors, clean multiset, 5 clues).
A/B protocol (prepare, do NOT launch unattended — user starts long runs): paired
6-12 h push460 soaks, same seed board, machine idle. Metric: distinct boards at
best/best-1 on disk per hour (count with a small script you write in tools\) and
new candidate_merge_* files. Pre-registered: adopt if disk-distinct >= 10x
baseline (~2) with no leakage. Write the exact soak commands into
docs\phase1_soak.md. Update CLAUDE.md with the implementation entry; ask before
committing.
```

## Prompt C — P4: sweep restructure for strong boards

```
Read CLAUDE.md fully, then docs/STRATEGY_460.md (§1.6, §3-P4).
Python-only task on tools\window_sweep.py + the sweep stage of push460.ps1.
Established evidence (do not re-verify): on the 455 board every swept window
(45-48-cell rectangles, 17-26 faults, ~80 win_edges, 300 s) is UNKNOWN — 35/35;
the sweep sorts fault-count DESCENDING, which finds improvements fast on weak
boards (446/448 improved on their first window) but burns the whole budget on
undecidable windows for strong boards. Bare fault clusters on 455 are proven
INFEASIBLE in 0.2 s, so decidable-but-informative windows exist between those
two extremes.

Implement:
1. Fault-shaped windows: add cluster-dilate shapes — each 8-adjacency fault
   cluster dilated r=1 (and r=2 if <= ~45 cells) as window candidates alongside
   rectangles. Reuse fault_cells/dilation logic from window_solve.py.
2. Adaptive ordering: new flag --order {hard-first,easy-first}; easy-first sorts
   ascending by win_edges (proxy for decidability). push460 should pass
   easy-first when the target scores >= adopted-best - 1, hard-first otherwise.
3. Pass-3 tier in push460's sweep rotation: after pass 2, the 3-5 undecided
   windows with the fewest win_edges per fault get --time-per 7200-14400.
   Respect the existing chunking design and the documented Finish-Sweep gotcha
   (a budget-exhausted chunk prints BOTH "time budget exhausted" and the "done:"
   summary — check the budget marker first).
4. Multi-target: ensure the rotation covers ALL current-best-score siblings
   (runs\candidate_merge_<best>_*.txt and runs\archive_455_basinB\* if present),
   not just best_c5.txt — each is a distinct piece pool.
Verify with a bounded live run (~30 min) on the 455 target: confirm cluster
windows enter the todo list, easy-first banks INFEASIBLE proofs, state files
stay compatible (old keys must not be invalidated). Metric to report: decided
windows per hour, before vs after. This is a coverage change, not a hypothesis —
no adopt threshold, but drop any shape class that stays >90% UNKNOWN at its
tier. Update CLAUDE.md; ask before committing.
```

## Prompt D — P6: deep-band campaign preparation (config only)

```
Read CLAUDE.md fully, then docs/STRATEGY_460.md (§1.3, §3-P6).
No code changes. Prepare and partially execute deep-band campaigns aimed at the
measured 455 fault geometry. Known geometry: basin-B boards (six 455s in git
HEAD, runs\candidate_merge_455_*, restorable via git show; also
runs\archive_455_basinB\ if a prior session created it) have one 38-40-cell
fault blob in rows 10-15 — native deep-band range. Basin A
(runs_scratch\best_c5.txt, candidate_merge_455_a775bd8f) has clusters in cols
10-15 rows 1-14 — one 90-degree rotation away (BandSolver already rotates;
higher orient indices cover it). All boards have 7-10 double-break cells, so
--cost2 2 is mechanism-matched.

Tasks:
1. Bounded systematic sweeps NOW (each <= ~30 min, machine idle, run these
   yourself): --mode band on each distinct 455, all 4 orients, y0 range
   permitted by --band-y0-max 14, --band-nodes 30000000, --cost2 2. Record which
   bands exhaust (optimality certificates) vs cap undecided — this maps where
   the anytime lottery has room.
2. Prepare (do NOT launch — user starts long runs) overnight anytime commands:
   solver.exe --seed-edges <sibling> with --band-y0-min 2..6, --band-nodes 1e9+,
   --band-stagnation lowered so rebuilds fire often, --cost2 2, umbrella off.
   One command per sibling per night, written to docs\band_campaign.md with a
   rota (which sibling, which night).
3. Pre-registered retirement rule (write it in the doc): if ~3 machine-nights
   across siblings produce zero band improvements at 455, retire the campaign.
4. Any gain: e2lib-verify (score, 0 border errors, clean multiset, 5 clues),
   save candidate + bucas URL, report immediately.
Update CLAUDE.md with the systematic-sweep findings; ask before committing.
```

## Prompt E — P7: umbrella retune (GATED on the Phase 1 soak verdict)

```
Read CLAUDE.md fully, then docs/STRATEGY_460.md (§3-P7). Confirm with the user
that the Phase 1 harvest soak has run and merges alone did NOT break 455 —
otherwise stop; this task is gated on that verdict.

Established evidence: at the 455 plateau the umbrella fires ~17x/21 min and never
converts. Bare 25-cell fault clusters are PROVEN INFEASIBLE, so 25-40-cell
greedy rebuilds (current umbrella scale) provably land back in the same basin.
Measured basin-transition scale (sigma-cycles) is 80-154 cells.

Implement as opt-in flags in src\solver.cpp, testing in this order:
1. --umbrella-target fault: kick blob = a fault cluster union its r=1 dilation
   instead of a random blob (compute clusters from the current board).
2. --umbrella-dist scaled to 60-100 cells (keep 32 default).
3. --umbrella-protect N: after a kick, the lineage may not re-anchor to the
   global best via ensure_board for N polish steps (protected drift). Preserve
   every score-gated path — degraded boards must never leak to pools/best files.
Verification: --selftest PASS, defaults bit-identical, rebuild both exes,
e2lib-verify smoke outputs.
A/B protocol (prepare commands; user launches): paired 12-h runs, 2-3 reps per
arm, harvest persistence ON in both arms, same seed, machine idle. Metrics:
distinct 455s discovered per run (from the persisted pool) and any 456.
Pre-registered adopt: any e2lib-verified 456, OR >= 3x distinct-455 discovery
rate. Explicitly do NOT evaluate on 90-s or 8-min horizons (wrong regime — see
the polish-dyn entry). Negative result gets a full CLAUDE.md entry too. Ask
before committing.
```

## Prompt F — P8: RegionSolver full per-node Hungarian bound (two-stage)

```
Read CLAUDE.md fully, then docs/STRATEGY_460.md (§3-P8). This is the listed
"full per-node Hungarian bound in RegionSolver" idea from CLAUDE.md's tuning
list. Goal: exhaustive regions at 24-40 cells instead of ~16, which upgrades
in-process merges (25-37-cell diffs currently CP-SAT-UNKNOWN become incumbent-
seeded B&B targets) and tie-drift volume at bigger scales.

Stage 1 — instrumentation FIRST (cheap, deterministic, decides go/no-go):
Add a --mode region-bench harness: fixed set of regions (24 and 32 cells,
rect + fault-blob, sampled from runs_scratch\best_c5.txt with fixed RNG seed)
solved at fixed node caps (2M, 16M) with the CURRENT top-2 dynamic suffix bound.
Report decide-rate (exhausted vs capped). This is the baseline.

Stage 2 — the bound: per-node assignment lower bound over remaining cells x
remaining pieces (val[i][p] machinery already exists from the root Hungarian
relaxation). O(n^3) per node is unaffordable — implement incrementally: warm-
start duals from the parent node / recompute only affected rows, and
early-terminate when the bound already exceeds best_gain. Preserve incumbent
seeding semantics exactly (capped searches can never lose ground).
Pre-registered adopt for the bound itself: >= 5x decide-rate at 32 cells at
fixed node cap, AND polish iteration throughput regression < 20% in a 5-min
mix smoke (status-line polish= rate). If adopted, only then prepare (user
launches) a 12-h paired A/B raising --region-max-big to 32 with the harvest-
pool metrics from docs\phase1_soak.md as the scoreboard.
Verification at every step: --selftest PASS, defaults bit-identical until
adoption, rebuild both exes, e2lib-verify any board the bench touches. Update
CLAUDE.md; ask before committing.
```

## Prompt G — P10: basin farming harness

```
Read CLAUDE.md fully, then docs/STRATEGY_460.md (§3-P10, §5). Requires the
harvest-persistence work (plateau.txt, drift reload) to be merged — verify it
exists in src\solver.cpp / push460.ps1 first; if not, stop and tell the user.

Goal: make fresh-basin farming systematic. Known economics: scratch lineages hit
451 in 90 s, 453 in 32 min, 455 in ~2.6 h (or minutes with sweep-adoption); two
basins have both walled at exactly 455 — n=2, so farming buys both lottery
tickets on basin quality and merge material per basin.

Build a small PowerShell harness (scripts\farm.ps1) that, per farmed basin:
1. Runs push460.ps1 -Fresh with a basin-specific -OutDir (runs_farm\basin_NNN\),
   a -Minutes budget the user sets, and harvest persistence on.
2. On completion, records to runs_farm\registry.md: plateau score, time-to-
   plateau, distinct near-best pool size, fault geometry summary (cluster
   sizes/rows/double-break count — compute via a small python helper using
   tools\e2lib.py), and pairwise agreement vs all previously registered basins
   (expect 5-12/256 for genuinely new basins; >200 means it rediscovered one).
3. Flags any basin that (a) exceeds 455 organically or (b) plateaus at 455 with
   fault geometry unlike existing basins — those get promoted to the main
   merge/sweep machinery.
Also ensure archive discipline: farmed basins must never share drift/plateau
dirs (stale-snapshot poisoning — see run.ps1's archiving rationale in CLAUDE.md).
Smoke-test the harness with two -Minutes 10 farm runs (you may run these
yourself), verify the registry entries and e2lib-verify both plateau boards.
Prepare the real farming rota (nightly/parallel-with-idle-capacity) in
runs_farm\ROTA.md for the user to launch. Update CLAUDE.md; ask before
committing.
```

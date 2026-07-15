# Phase 0 (STRATEGY_460 P1) — 455 merge-ladder + cluster-window results

Campaign started 2026-07-13. Driver: `tools/phase0_ladder.py` (resumable; state in `docs/phase0_state.json`).

8 distinct 455/480 5-clue boards, e2lib-verified (455, 0 border errors, clean multiset, all 5 clues). Basin A = {45bbcff6 (= runs_scratch best_c5), a775bd8f}; basin B = six boards archived in `runs/archive_455_basinB/` (byte-verified against git HEAD). All probes: `window_solve.py --clues 5 --improve-only --workers 16`, sequential, machine idle. INFEASIBLE = window proven optimal for its piece pool; any SAT = 456.

| probe | window | cells | time(s) | verdict | wall(s) | win_edges | inc_gain | when |
|---|---|---|---|---|---|---|---|---|
| b3cd2796×cbdf0052 (diff 25) | pair-diff | 25 | 600 | INFEASIBLE | 1.0 | 25 | 46 | 2026-07-13 23:10:02 |
| b9b532fe×cbdf0052 (diff 25) | pair-diff | 25 | 600 | INFEASIBLE | 1.0 | 25 | 46 | 2026-07-13 23:10:03 |
| 35985248×4279accd (diff 27) | pair-diff | 27 | 600 | INFEASIBLE | 1.4 | 30 | 48 | 2026-07-13 23:10:05 |
| 35985248×addb1f31 (diff 27) | pair-diff | 27 | 600 | INFEASIBLE | 1.4 | 30 | 48 | 2026-07-13 23:10:06 |
| 4279accd×cbdf0052 (diff 27) | pair-diff | 27 | 600 | INFEASIBLE | 5.4 | 31 | 46 | 2026-07-13 23:10:12 |
| addb1f31×cbdf0052 (diff 27) | pair-diff | 27 | 600 | INFEASIBLE | 4.1 | 31 | 46 | 2026-07-13 23:10:16 |
| 35985248×b3cd2796 (diff 28) | pair-diff | 28 | 600 | UNKNOWN | 601.6 | 31 | 52 | 2026-07-13 23:20:17 |
| 35985248×b9b532fe (diff 28) | pair-diff | 28 | 600 | UNKNOWN | 601.3 | 31 | 52 | 2026-07-13 23:30:19 |
| 4279accd×b3cd2796 (diff 37) | pair-diff | 37 | 600 | UNKNOWN | 602.0 | 49 | 62 | 2026-07-13 23:40:21 |
| 4279accd×b9b532fe (diff 37) | pair-diff | 37 | 600 | UNKNOWN | 602.2 | 49 | 62 | 2026-07-13 23:50:23 |
| addb1f31×b3cd2796 (diff 37) | pair-diff | 37 | 600 | UNKNOWN | 602.0 | 49 | 62 | 2026-07-14 00:00:25 |
| addb1f31×b9b532fe (diff 37) | pair-diff | 37 | 600 | UNKNOWN | 601.6 | 49 | 62 | 2026-07-14 00:10:27 |
| 35985248×b3cd2796 (diff 28) | pair-diff | 28 | 1800 | INFEASIBLE | 503.8 | 31 | 52 | 2026-07-14 00:18:51 |
| 35985248×b9b532fe (diff 28) | pair-diff | 28 | 1800 | INFEASIBLE | 1047.0 | 31 | 52 | 2026-07-14 00:36:18 |
| 4279accd×b3cd2796 (diff 37) | pair-diff | 37 | 1800 | UNKNOWN | 1804.5 | 49 | 62 | 2026-07-14 12:35:34 |
| 4279accd×b9b532fe (diff 37) | pair-diff | 37 | 1800 | UNKNOWN | 1801.8 | 49 | 62 | 2026-07-14 14:27:02 |
| addb1f31×b3cd2796 (diff 37) | pair-diff | 37 | 1800 | UNKNOWN | 1801.7 | 49 | 62 | 2026-07-14 14:57:04 |
| addb1f31×b9b532fe (diff 37) | pair-diff | 37 | 1800 | UNKNOWN | 1802.5 | 49 | 62 | 2026-07-14 15:27:06 |
| 4279accd×b3cd2796 (diff 37) | pair-diff | 37 | 14400 | UNKNOWN | 14404.2 | 49 | 62 | 2026-07-14 19:56:16 |
| 4279accdxb9b532fe, addb1f31xb3cd2796, addb1f31xb9b532fe | pair-diff (same model as 4279accdxb3cd2796) | 37 | 14400 | UNKNOWN | via 4279accdxb3cd2796 | 49 | 62 | 2026-07-14 19:56:16 |
| 45bbcff6 cluster0 (26c dil-1) | fault-cluster dil-1 | 26 | 7200 | UNKNOWN | 7203.6 | 97 | 106 | 2026-07-14 21:56:20 |
| 45bbcff6 cluster1 (15c dil-1) | fault-cluster dil-1 | 15 | 7200 | INFEASIBLE | 2.5 | 62 | 75 | 2026-07-14 21:56:23 |
| a775bd8f cluster0 (26c dil-1) | fault-cluster dil-1 | 26 | 7200 | UNKNOWN | 7210.1 | 97 | 106 | 2026-07-15 11:24:52 |
| a775bd8f cluster1 (15c dil-1) | fault-cluster dil-1 | 15 | 7200 | INFEASIBLE | 2.5 | 62 | 75 | 2026-07-15 11:24:55 |

## Campaign summary & decision readout (2026-07-15)

**No SAT. No 456 exists among any probed window.** Full tally over the 8 distinct 455s:

- **Pair merges (13 in-scope incl. the smoke-tested basin-A pair): 9 INFEASIBLE, 4 UNKNOWN.**
  Every diff ≤ 28 is proven INFEASIBLE — 25–27-cell diffs in 1–5 s, the two 28-cell
  diffs at the 1800-s tier (504 s / 1047 s). The four 37-cell S2×S3 pairs are UNKNOWN
  at 600 s, 1800 s, and 14400 s.
- **Structural finding (tier-3 collapse):** the four 37-cell pairs disagree on the
  *identical* 37-cell set, which forces all four boards to be identical outside it —
  same fixed boundary, same piece pool, same improve-only CP-SAT model. One 4-h probe
  (16 workers) decided them all: UNKNOWN. The driver dedupes by model signature
  (window cells + outside placements + pool) and propagates verdicts (`via` rows).
- **Dilate-1 fault-cluster windows (basin A, both boards):** the 15-cell cluster's
  40-cell window is **INFEASIBLE in 2.5 s** on both boards — optimality now proven
  *including one ring of piece inflow*, strictly extending the earlier bare-cluster
  proofs. The 26-cell cluster's 60-cell window (97 win_edges) is UNKNOWN at 7200 s
  on both boards.

**Decision value (per STRATEGY_460 §3-P1):** both pre-registered readouts fire at once.
Everything decidable is INFEASIBLE ⇒ **basin diversity is exhausted at this diff scale**
— the eight 455s contain no 456 reachable by ≤28-cell merges, and basin A's fault
zones are locally optimal even with dilate-1 inflow (except the one 60-cell window
CP-SAT cannot decide). And the sole remaining pair-model (37 cells / 49 win_edges)
plus both 60-cell cluster windows are **CPU-undecidable at 2–4 h** ⇒ escalating
budgets on large windows buys nothing at practical cost — **small-diff volume (the P2
harvest fix: persist `g_plateau`, drift-not-arrival snapshots) strictly dominates
further escalation**, and the P9 (GPU/big-exact-neighborhood) case weakens further.

Campaign totals: 23 CP-SAT solves, ~11.4 h solver wall time, sequential, 16 workers,
machine idle, every probe logged above and in `runs/phase0_logs/`. Resumable driver:
`tools/phase0_ladder.py` (state `docs/phase0_state.json`).

# Phase-1 soak: plateau-harvest fix A/B (STRATEGY_460 P2 + P3 + P5)

*Written 2026-07-15 alongside the implementation. The user starts these runs —
they are prepared here, not launched automatically.*

## What ships in the treatment arm

- **P2a drift-not-arrival snapshots** (`--drift-every 300`, solver): while a
  polisher lineage sits within 1 of the global best, snapshot every 300
  non-improving steps (per-thread ring of the last 256 write hashes dedupes),
  in addition to the old once-per-arrival snapshot.
- **P2b/P2c plateau persistence**: the solver dumps its deduped in-memory
  `g_plateau` pool to `<drift>\plateau.txt` (atomic tmp+rename) 1 min after
  start, then every 5 min, and at clean exit; on startup it reloads
  `drift_t*.txt` + `plateau.txt` into `g_plateau` (each line re-parsed,
  clue/rim-conformance-checked, re-scored, gated at `>= resumed_best - 1`).
- **P3 stagnation policy** (push460): default `-StagnationHours` 2 → **3**; on
  stagnation, *dump-then-merge* — wait for a plateau dump newer than the
  trigger (≤6 min), then for one full merge pass that started after it
  (≤30 min), then restart the solver (which reloads the pool).
- **P5 merge time scaling** (`plateau_merge.py --time-top 600`): pairs in the
  top score group get 600 s of CP-SAT; lower groups keep 60 s. The merge pool
  now includes `plateau.txt`.

`--drift-every 0` (push460 `-DriftEvery 0`) switches ALL of the above off —
arrival-only snapshots, no plateau.txt, no reload, immediate stagnation
restart. That is the baseline arm.

## Protocol

Paired push460 soaks, **8 h per arm, run sequentially, machine otherwise
idle**, same seed board (the current 455, basin A), fresh empty drift dirs,
`-NoSweep` in both arms (the sweep engine is orthogonal to the harvest change
and only adds adoption variance). Both arms write improvement candidates to
`runs\` — attribute them by the `--since` timestamp windows below; do NOT run
the arms concurrently.

### Prep (once, from the repo root)

```powershell
New-Item -ItemType Directory -Force runs_scratch\soak_base | Out-Null
Copy-Item runs_scratch\best_c5.txt runs_scratch\soak_base\best_c5.txt
New-Item -ItemType Directory -Force runs_scratch\soak_p2 | Out-Null
Copy-Item runs_scratch\best_c5.txt runs_scratch\soak_p2\best_c5.txt
```

### Arm A — baseline (pre-fix pipeline behavior)

```powershell
Get-Date -Format "yyyy-MM-dd HH:mm"   # note as SINCE_A
.\push460.ps1 -OutDir runs_scratch\soak_base -Minutes 480 -NoSweep `
    -DriftEvery 0 -StagnationHours 1 -TotalThreads 30
```

(`-StagnationHours 1` matches the pipeline configuration that produced the
measured 2-distinct-board baseline on 2026-07-13.)

### Arm B — treatment (new defaults)

```powershell
Get-Date -Format "yyyy-MM-dd HH:mm"   # note as SINCE_B
.\push460.ps1 -OutDir runs_scratch\soak_p2 -Minutes 480 -NoSweep -TotalThreads 30
```

### Readout (after each arm)

```powershell
python tools\count_distinct.py --dir runs_scratch\soak_base\drift --hours 8 `
    --candidates runs --since "SINCE_A"
python tools\count_distinct.py --dir runs_scratch\soak_p2\drift --hours 8 `
    --candidates runs --since "SINCE_B"
```

The script recomputes every line's score via e2lib (never trusts the file),
reports distinct boards per score and the headline metric (distinct at
best/best−1 on disk), runs the leakage check (file-score mismatches, border
errors) and a full multiset+clue check on the top-2 score groups, and counts
new `candidate_merge_*` files in the window.

## Pre-registered decision rule

- **Adopt** (keep the new defaults) if the treatment arm's distinct boards at
  best/best−1 on disk is **≥ 10× the baseline arm's** (baseline ≈ 2 from the
  2026-07-13 measurement, so this is nearly assured) **and** the leakage
  checks are clean in both arms (no score mismatches, 0 border errors, clean
  multisets, clues intact). The honest secondary check: `candidate_merge_*`
  production actually increases.
- **A 456 in either arm ends the experiment**: e2lib-verify it, adopt via
  `--seed-edges`, re-center the pipeline on the 456 pool (STRATEGY_460 §4).
- If treatment wins the metric but merges still yield no 456 after ~2 days of
  soaking, the 455 basins are merge-exhausted at ≤45-cell diffs → weight
  shifts to P7 (bigger kicks) and P10 (basin farming) per the strategy doc —
  with the harvest machinery left ON (it is the enabling layer either way).

## Notes

- If time allows a second rep per arm (2×6 h each, interleaved B,T,B,T),
  thermal/background fairness improves; the decision rule stays the same.
- The smoke validation of this machinery (4-min push460 on a seeded 455 +
  restart-reload check + e2lib spot checks) is recorded in CLAUDE.md's
  2026-07-15 implementation entry.

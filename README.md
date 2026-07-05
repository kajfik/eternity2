# Eternity II solver

Attempts to beat the current partial-solution records for the Eternity II
puzzle: **461/480** matched edges with all 5 clues placed, or **470/480** with
only the center clue (piece 139). Solutions are emitted as
[e2.bucas.name](https://e2.bucas.name/) viewer links.

## Quick start

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1   # needs g++ (WinLibs MinGW)
powershell -ExecutionPolicy Bypass -File scripts\run5.ps1    # 5-clue hunt, runs until Ctrl+C
powershell -ExecutionPolicy Bypass -File scripts\run1.ps1    # 1-clue hunt
```

Both scripts resume automatically from `runs\best_c{5,1}.txt`, which is
rewritten on every new best and contains the placement grid, the 1024-letter
edge string, and the bucas viewer URL. `runs\history_c{5,1}.log` appends one
line (timestamp, score, URL) per improvement. A timed run:
`scripts\run5.ps1 -Minutes 30`.

## How it works (`src/solver.cpp`, single file, C++20)

Two cooperating move types run on all cores (`--mode mix`, the default —
`hunt` / `polish` run one type alone):

- **Hunter** — randomized perfect border-ring construction, then row-major
  interior DFS under a mismatch budget with exact candidate buckets
  `[west][north]`, a color-deficit lower bound, and anytime branch-and-bound
  (finishing a board tightens the budget to `cost − 1`, so every completion
  strictly improves that lineage).
- **Polisher (LNS)** — takes a completed board from a seed pool and repeatedly
  re-solves small regions *exactly*: fault-blob or rectangle regions of up to
  `--region-max` (16) cells via an incumbent-seeded branch-and-bound
  (`RegionSolver`) that starts from the current arrangement, so a capped
  search can never lose ground; plus a Hungarian-algorithm optimal
  reassignment of pairwise non-adjacent fault cells.

Two seed pools connect them: an **elite** pool (within `--seed-slack` of the
global best) and a **fresh** pool sampling all completions, which polishers
draw from 30% of the time so the search doesn't get trapped polishing one
lineage's basin.

Clue cells are pinned and the outer rim is always grey-correct, so every
reported score is for a valid record-eligible board.

## Ground truth & validation

- `tools/e2lib.py` — piece data parsed from the viewer's own
  `reference/bucas_init.js`, URL build/parse, and an independent scorer
  (validated to reproduce the published 467–470 reference boards exactly).
- `tools/make_data.py` — regenerates `data/` and `src/e2data.h` from the
  reference, re-checking all invariants.
- Solver output cross-checked: C++ score, Python score, piece multiset, clue
  placement and grey border all agree.

## Useful flags

`--clues 5|1`, `--threads N` (default: cores − 2), `--minutes M`,
`--seed-edges <bucas URL>` (inject an external board into the pools),
`--region-max`, `--region-nodes`, `--polish-frac`, `--seed-slack`.

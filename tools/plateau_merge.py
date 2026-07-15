"""Plateau-merge driver (task #5 Part B).

The C++ solver's Polisher can snapshot same-lineage sideways-drift variants of
near-best boards to <drift_dir>/drift_t<thread_id>.txt (see --drift-dir in
src/solver.cpp, Part A of this feature). Two boards that share a lineage but
drifted apart typically disagree only on a small "disagreement set" of cells
(the two arrangements agree everywhere else). This driver:

  1. Collects all snapshot lines from drift_t*.txt in --dir, plus
     plateau.txt (the solver's persisted in-memory g_plateau pool,
     dumped every 5 min / at clean exit -- 2026-07-15 harvest fix).
  2. Groups them by score, keeps the top 2 score groups (the most promising
     near-best plateaus).
  3. For every pair within a group (including across different thread files),
     computes the disagreement cell set (cells where the 4-char quad differs).
     Pairs with a disagreement too small (<4, trivial/no-op) or too large
     (>--max-window, CP-SAT window too big to be practical) are skipped.
  4. Solves each remaining pair's disagreement window exactly via CP-SAT
     (window_solve.solve_window, imported directly -- no subprocess), asking
     only "does a strictly better arrangement of this window exist?"
     (improve_only=True). Since everything outside the window is identical
     between the two boards and unions of two feasible boards' piece pools
     restricted to the window are just that window's own multiset, this is a
     safe, exact, strict full-board improvement test.
  5. On an improved result, re-validates independently via e2lib (score,
     piece multiset, border) and writes runs/candidate_merge_<score>_<hash8>.txt
     with the bucas URL (hash8 = md5 prefix of the edges, so distinct
     same-score boards -- the point of plateau harvesting -- never clobber).

Dedupe: a fingerprint of each solved pair (hash of both edge strings, order
independent) is cached in <dir>/.plateau_merge.state (newline-delimited JSON,
same convention as window_sweep.py's sweep state file) so repeated --loop
invocations do not re-solve the same pair.

--include FILE (repeatable) adds a board from a solver best file (the
save_board format written to runs*/best_c*.txt: '# ...' header, piece:rot
grid, bare 1024-char edges line, bucas URL -- or any file containing either
of the last two) to the snapshot pool, so the current best board itself gets
paired against its drift snapshots. Its score is computed via e2lib, not
trusted from the file. scripts/merge.ps1 is the launcher wrapping this.

Usage (from tools/, or anywhere -- paths are resolved via e2lib.ROOT):
  python plateau_merge.py --dir ../runs/drift --clues 5 --time 600 --workers 16
  python plateau_merge.py --dir ../runs/drift --clues 5 --loop
  python plateau_merge.py --dir ../runs_scratch/drift --clues 5 \
      --include ../runs_scratch/best_c5.txt
"""

import argparse
import glob
import hashlib
import json
import os
import random
import time
from collections import defaultdict

import e2lib
from window_solve import solve_window, CLUES_5, CLUES_1, W, H

NC = W * H


def load_snapshots(drift_dir):
    """Read every drift_t*.txt (plus plateau.txt, the solver's persisted
    g_plateau dump, 2026-07-15) in drift_dir. Returns list of
    (score, edges, src)."""
    paths = sorted(glob.glob(os.path.join(drift_dir, "drift_t*.txt")))
    plateau = os.path.join(drift_dir, "plateau.txt")
    if os.path.exists(plateau):
        paths.append(plateau)
    out = []
    for path in paths:
        src = os.path.basename(path)
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split(None, 1)
                if len(parts) != 2:
                    continue
                score_s, edges = parts
                if len(edges) != NC * 4:
                    continue
                try:
                    score = int(score_s)
                except ValueError:
                    continue
                out.append((score, edges, src))
    return out


def load_include(path):
    """Load one board from a solver best file (save_board format) or any file
    containing a bare 1024-char edges line or a bucas URL. Returns a
    (score, edges, src) tuple shaped like a drift snapshot; the score is
    recomputed via e2lib rather than trusted from the file."""
    edges = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if len(line) == NC * 4 and line.isalpha():
                edges = line
                break
            if "board_edges=" in line:
                edges = e2lib.parse_url(line)["board_edges"]
                break
    if edges is None or len(edges) != NC * 4:
        raise SystemExit(f"--include {path}: no {NC * 4}-char edges line or "
                         f"bucas URL found")
    score, _conflicts, _border_errors = e2lib.score_edges(edges)
    return score, edges, os.path.basename(path)


def disagreement_cells(edges_a, edges_b):
    return {s for s in range(NC)
            if edges_a[4 * s:4 * s + 4] != edges_b[4 * s:4 * s + 4]}


def pair_fingerprint(edges_a, edges_b):
    """Order-independent hash identifying an unordered pair of boards."""
    ha = hashlib.md5(edges_a.encode()).hexdigest()
    hb = hashlib.md5(edges_b.encode()).hexdigest()
    lo, hi = sorted((ha, hb))
    return hashlib.md5((lo + hi).encode()).hexdigest()


def load_state(state_path):
    done = {}
    if os.path.exists(state_path):
        with open(state_path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                rec = json.loads(line)
                done[rec["fp"]] = rec
    return done


def append_state(state_path, rec):
    with open(state_path, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")


def build_pairs(snapshots, max_window, done, max_boards=0, keep_srcs=()):
    """Group by score, keep top-2 score groups, enumerate candidate pairs
    (across threads too), ordered by disagreement size ascending.

    Scaled for large drift pools (--drift-cap in the thousands): dedupe is
    O(n) via a set, already-solved pairs (fingerprint in `done`) are skipped
    BEFORE the O(256) disagreement diff (per-board md5 cached once), and a
    group larger than max_boards is randomly subsampled (boards whose src is
    in keep_srcs -- the --include boards -- always survive sampling)."""
    by_score = defaultdict(list)
    for score, edges, src in snapshots:
        by_score[score].append((edges, src))
    top_scores = sorted(by_score, reverse=True)[:2]

    pairs = []
    n_done = 0
    for score in top_scores:
        seen = set()
        uniq = []
        for edges, src in by_score[score]:
            if edges not in seen:
                seen.add(edges)
                uniq.append((edges, src))
        if max_boards and len(uniq) > max_boards:
            kept = [e for e in uniq if e[1] in keep_srcs]
            rest = [e for e in uniq if e[1] not in keep_srcs]
            uniq = kept + random.sample(rest, max_boards - len(kept))
            print(f"[plateau_merge] score {score}: sampled {len(uniq)} of "
                  f"{len(seen)} distinct boards (--max-boards {max_boards})")
        md5 = {edges: hashlib.md5(edges.encode()).hexdigest()
               for edges, _ in uniq}
        n = len(uniq)
        for i in range(n):
            for j in range(i + 1, n):
                ea, sa = uniq[i]
                eb, sb = uniq[j]
                lo, hi = sorted((md5[ea], md5[eb]))
                fp = hashlib.md5((lo + hi).encode()).hexdigest()
                if fp in done:
                    n_done += 1
                    continue
                diff = disagreement_cells(ea, eb)
                if len(diff) < 4 or len(diff) > max_window:
                    continue
                pairs.append((len(diff), score, ea, sa, eb, sb, diff))
    pairs.sort(key=lambda t: t[0])
    return pairs, n_done


def run_once(args, clue_map):
    snapshots = load_snapshots(args.dir)
    keep_srcs = set()
    for path in args.include:
        score, edges, src = load_include(path)
        print(f"[plateau_merge] --include {path}: score {score}")
        snapshots.append((score, edges, src))
        keep_srcs.add(src)
    if not snapshots:
        print(f"[plateau_merge] no snapshot lines found in {args.dir}")
        return 0
    scores = sorted({s for s, _, _ in snapshots}, reverse=True)
    top_score = scores[0]
    print(f"[plateau_merge] {len(snapshots)} snapshot lines, scores present: "
          f"{scores[:5]}{'...' if len(scores) > 5 else ''}")

    state_path = os.path.join(args.dir, ".plateau_merge.state")
    done = load_state(state_path)

    pairs, n_skipped = build_pairs(snapshots, args.max_window, done,
                                   args.max_boards, keep_srcs)
    print(f"[plateau_merge] {len(pairs)} candidate pairs after top-2-score "
          f"grouping, dedupe against state ({n_skipped} already solved) and "
          f"disagreement-size filter (4..{args.max_window})")

    t_end = time.time() + args.time if args.time else None
    n_solved = n_improved = 0
    for ndiff, score, ea, sa, eb, sb, diff in pairs:
        fp = pair_fingerprint(ea, eb)
        if fp in done:
            n_skipped += 1
            continue
        if t_end and time.time() > t_end:
            print("[plateau_merge] time budget exhausted")
            break
        # per-score-group time scaling (2026-07-15, STRATEGY_460 P5): the top
        # score group is where +1 lives -- observed 22-cell 454-pairs decide
        # in 20-32 s while 21+-cell pairs go UNKNOWN at 60 s, i.e. the default
        # budget sits right at the decision edge. Lower groups keep the short
        # budget so they can't starve the pass.
        tp = args.time_top if score >= top_score else args.time_per_pair
        print(f"[plateau_merge] pair {sa}<->{sb} score={score} "
              f"disagreement={ndiff} cells budget={tp:g}s", flush=True)
        sname, new_edges, inc, best = solve_window(
            ea, diff, clue_map, time_s=tp,
            workers=args.workers, improve_only=True)
        n_solved += 1
        rec = {"fp": fp, "verdict": sname, "score": score, "ndiff": ndiff,
               "src_a": sa, "src_b": sb}
        if sname in ("OPTIMAL", "FEASIBLE") and new_edges:
            s1, confl, be1 = e2lib.score_edges(new_edges)
            data = e2lib.E2Data()
            e2lib.check_piece_set(data, new_edges)  # raises on duplicate pieces
            if be1 != 0 or s1 <= score:
                print(f"[plateau_merge] WARNING: solver claimed improvement "
                      f"but validation disagrees (score={s1} border_errors="
                      f"{be1} baseline={score}); discarding")
            else:
                n_improved += 1
                url = e2lib.build_url(new_edges, name="E2Solver", data=data)
                # fingerprint suffix: distinct same-score boards are the whole
                # point of plateau harvesting -- don't clobber them
                bh = hashlib.md5(new_edges.encode()).hexdigest()[:8]
                out_path = os.path.join(e2lib.ROOT, "runs",
                                         f"candidate_merge_{s1}_{bh}.txt")
                with open(out_path, "w", encoding="utf-8") as f:
                    f.write(f"# plateau_merge improvement {score} -> {s1} "
                            f"(pair {sa}<->{sb}, disagreement {ndiff} cells)\n"
                            f"{url}\n")
                print("*" * 70)
                print(f"*** IMPROVED {score} -> {s1}: wrote {out_path} ***")
                print(url)
                print("*" * 70)
                rec["result_score"] = s1
        append_state(state_path, rec)
        done[fp] = rec

    print(f"[plateau_merge] done: solved={n_solved} skipped(dedupe)={n_skipped} "
          f"improved={n_improved}")
    return n_improved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, help="drift snapshot directory "
                    "(matches --drift-dir passed to the C++ solver)")
    ap.add_argument("--clues", type=int, choices=(1, 5), required=True)
    ap.add_argument("--time", type=float, default=600,
                    help="overall wall-clock budget per invocation (s); "
                         "0 = no cap (solve every pending pair)")
    ap.add_argument("--time-per-pair", type=float, default=60,
                    help="CP-SAT time cap per pair (s) for score groups "
                         "below the top one")
    ap.add_argument("--time-top", type=float, default=600,
                    help="CP-SAT time cap per pair (s) for the TOP score "
                         "group -- where +1 lives; observed 22-cell top "
                         "pairs decide in 20-32 s but 21+-cell pairs go "
                         "UNKNOWN at 60 s, so the top group gets 10x")
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--max-window", type=int, default=45,
                    help="skip pairs whose disagreement set exceeds this "
                         "many cells (undecidable in practice)")
    ap.add_argument("--max-boards", type=int, default=0,
                    help="cap per score group: subsample this many distinct "
                         "boards before pairing (0 = unlimited). Keeps pass "
                         "startup O(max_boards^2) when --drift-cap harvests "
                         "thousands of snapshots; --include boards always "
                         "survive the sampling")
    ap.add_argument("--include", action="append", default=[],
                    help="also add this board file (solver best_c*.txt "
                         "format, or any file with a bare edges line or "
                         "bucas URL) to the snapshot pool; repeatable; "
                         "re-read every pass under --loop")
    ap.add_argument("--loop", action="store_true",
                    help="re-run continuously, picking up new drift "
                         "snapshots each pass; dedupe state persists "
                         "across passes so solved pairs aren't repeated")
    args = ap.parse_args()
    clue_map = CLUES_5 if args.clues == 5 else CLUES_1

    if not args.loop:
        run_once(args, clue_map)
        return

    while True:
        improved = run_once(args, clue_map)
        if improved:
            print("[plateau_merge] improvement found; continuing loop")
        print("[plateau_merge] --loop: sleeping 30s before next pass")
        time.sleep(30)


if __name__ == "__main__":
    main()

"""Distinct-board census of a drift harvest -- the P2 (plateau-harvest fix)
A/B metric script (STRATEGY_460 / docs/phase1_soak.md, 2026-07-15).

Reads every drift_t*.txt plus plateau.txt in --dir, recomputes every line's
score via e2lib (file scores are never trusted), and reports:

  - distinct boards per score (top groups), optionally as a per-hour rate
    (--hours = soak duration);
  - the headline metric: distinct boards at best and best-1 ON DISK;
  - a leakage check: any line whose file score disagrees with the e2lib
    recompute, has border errors, a broken piece multiset, or wrong clues
    (top-2 score groups get the full multiset+clue check);
  - optionally (--candidates DIR [--since "YYYY-MM-DD HH:MM"]) the
    candidate_merge_* files produced, the soak's secondary metric.

Usage:
  python tools\\count_distinct.py --dir runs_scratch\\drift --hours 12 \
      --candidates runs --since "2026-07-15 20:00"
"""

import argparse
import glob
import os
from collections import defaultdict
from datetime import datetime

import e2lib

# clue placements (0-based cell, 1-based piece, rotation) -- E2 ground truth
CLUES_5 = {34: (208, 1), 45: (255, 3), 135: (139, 0), 210: (181, 3), 221: (249, 1)}
CLUES_1 = {135: (139, 0)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True,
                    help="drift dir (reads drift_t*.txt + plateau.txt)")
    ap.add_argument("--clues", type=int, choices=(1, 5), default=5)
    ap.add_argument("--hours", type=float, default=0,
                    help="soak duration; prints distinct/hour when set")
    ap.add_argument("--candidates", default=None,
                    help="also count candidate_merge_* files in this dir")
    ap.add_argument("--since", default=None,
                    help='only count candidates newer than "YYYY-MM-DD HH:MM"')
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.dir, "drift_t*.txt")))
    plateau = os.path.join(args.dir, "plateau.txt")
    if os.path.exists(plateau):
        paths.append(plateau)

    n_lines = mislabeled = border_bad = 0
    by_score = defaultdict(set)
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for line in f:
                parts = line.split(None, 1)
                if len(parts) != 2:
                    continue
                fscore, edges = parts[0], parts[1].strip()
                if len(edges) != 1024:
                    continue
                n_lines += 1
                score, _confl, berr = e2lib.score_edges(edges)
                if berr:
                    border_bad += 1
                try:
                    if int(fscore) != score:
                        mislabeled += 1
                except ValueError:
                    mislabeled += 1
                by_score[score].add(edges)

    if not by_score:
        print(f"no snapshot lines under {args.dir}")
        return
    scores = sorted(by_score, reverse=True)
    best = scores[0]
    print(f"{n_lines} snapshot lines in {len(paths)} files under {args.dir}")
    for s in scores[:8]:
        n = len(by_score[s])
        rate = f"  ({n / args.hours:.1f}/h)" if args.hours else ""
        print(f"  score {s}: {n} distinct{rate}")
    top2 = len(by_score[best]) + len(by_score.get(best - 1, ()))
    rate = f" = {top2 / args.hours:.2f}/h" if args.hours else ""
    print(f"METRIC distinct at {best}/{best - 1}: {top2}{rate}")

    if mislabeled or border_bad:
        print(f"WARNING: {mislabeled} lines with file score != e2lib recompute, "
              f"{border_bad} with border errors -- LEAKAGE CHECK FAILED")
    else:
        print("leakage check: all file scores match e2lib recompute, 0 border errors")

    # full multiset + clue check on the top-2 score groups (the merge fodder)
    data = e2lib.E2Data()
    clues = CLUES_5 if args.clues == 5 else CLUES_1
    checked = bad = 0
    for s in scores[:2]:
        for edges in by_score[s]:
            checked += 1
            try:
                placements = e2lib.check_piece_set(data, edges)
            except ValueError as ex:
                bad += 1
                print(f"  BAD multiset at score {s}: {ex}")
                continue
            for cell, (p, r) in clues.items():
                if placements[cell] != (p, r):
                    bad += 1
                    print(f"  BAD clue at score {s}: cell {cell} holds "
                          f"{placements[cell]}, want {(p, r)}")
                    break
    print(f"multiset+clue check on top-2 groups: {checked} boards, {bad} bad")

    if args.candidates:
        since = datetime.min
        if args.since:
            since = datetime.fromisoformat(args.since)
        cands = [p for p in glob.glob(os.path.join(args.candidates,
                                                   "candidate_merge_*.txt"))
                 if datetime.fromtimestamp(os.path.getmtime(p)) > since]
        tag = f" since {args.since}" if args.since else ""
        print(f"candidate_merge_* in {args.candidates}{tag}: {len(cands)}")
        for p in sorted(cands):
            print(f"  {os.path.basename(p)}")


if __name__ == "__main__":
    main()

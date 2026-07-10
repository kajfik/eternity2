"""Sliding-window improve-only sweep over an E2 board via CP-SAT.

Sweeps rectangular windows that touch at least one fault cell, asking for each:
"does ANY strictly better arrangement of this window's pieces exist?" —
INFEASIBLE answers are proofs of window-local optimality; a single SAT answer
is a strict full-board improvement (on a record board: a new world record).

Windows deliberately mix fault cells with perfect territory: that is the piece
inflow the fault-only windows cannot express. Progress is checkpointed to a
state file so the sweep can be interrupted and resumed.

Typical overnight run (from tools/):
  python window_sweep.py --board ../runs/best_c1.txt --clues 1 \
      --shapes 8x6,6x8,9x5,5x9,11x4,4x11 --time-per 600 --workers 28
"""

import argparse
import hashlib
import json
import os
import time

from window_solve import load_edges, fault_cells, solve_window, CLUES_5, CLUES_1, W, H
import e2lib


def rects(shapes, faults):
    """All (x0,y0,w,h) of the given shapes containing >=1 fault cell,
    ordered by fault-cell count descending (most repairable first)."""
    out = []
    for w, h in shapes:
        for y0 in range(0, H - h + 1):
            for x0 in range(0, W - w + 1):
                cells = {y * W + x for y in range(y0, y0 + h)
                         for x in range(x0, x0 + w)}
                nf = len(cells & faults)
                if nf:
                    out.append((nf, x0, y0, w, h, cells))
    out.sort(key=lambda t: -t[0])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", required=True)
    ap.add_argument("--clues", type=int, choices=(1, 5), required=True)
    ap.add_argument("--shapes", default="8x6,6x8,9x5,5x9",
                    help="comma list of WxH window shapes")
    ap.add_argument("--time-per", type=float, default=600,
                    help="CP-SAT cap per window (s)")
    ap.add_argument("--minutes", type=float, default=0,
                    help="total budget in minutes (0 = run all windows)")
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--retry-unknown", action="store_true",
                    help="re-attempt windows previously left UNKNOWN")
    ap.add_argument("--out", default="../runs/candidate_sweep.txt")
    args = ap.parse_args()

    edges = load_edges(args.board)
    score0, _, be = e2lib.score_edges(edges)
    assert be == 0
    clue_map = CLUES_5 if args.clues == 5 else CLUES_1
    faults = fault_cells(edges)
    shapes = [tuple(map(int, s.split("x"))) for s in args.shapes.split(",")]
    todo = rects(shapes, faults)

    state_path = args.board + f".sweep_c{args.clues}.state"
    state = {}
    if os.path.exists(state_path):
        with open(state_path, encoding="utf-8") as f:
            state = {json.loads(l)["key"]: json.loads(l) for l in f if l.strip()}
    bhash = hashlib.md5(edges.encode()).hexdigest()[:10]

    print(f"[sweep] board score {score0}, {len(faults)} fault cells, "
          f"{len(todo)} windows to consider, {len(state)} already decided")
    t_end = time.time() + args.minutes * 60 if args.minutes else None
    n_proved = n_unknown = n_skipped = 0
    for nf, x0, y0, w, h, cells in todo:
        key = f"{bhash}:{x0},{y0},{w}x{h}"
        prev = state.get(key)
        if prev and (prev["verdict"] != "UNKNOWN" or not args.retry_unknown):
            n_skipped += 1
            continue
        if t_end and time.time() > t_end:
            print("[sweep] time budget exhausted")
            break
        print(f"[sweep] window ({x0},{y0}) {w}x{h} faults={nf}", flush=True)
        sname, new_edges, inc, best = solve_window(
            edges, cells, clue_map, time_s=args.time_per,
            workers=args.workers, improve_only=True)
        with open(state_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"key": key, "verdict": sname,
                                "faults": nf}) + "\n")
        if sname in ("OPTIMAL", "FEASIBLE") and new_edges:
            s1, _, be1 = e2lib.score_edges(new_edges)
            data = e2lib.E2Data()
            e2lib.check_piece_set(data, new_edges)
            assert be1 == 0 and s1 > score0, "solver returned non-improvement?"
            url = e2lib.build_url(new_edges, name="E2Solver", data=data)
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(f"# window_sweep improvement {score0} -> {s1} "
                        f"(window {x0},{y0} {w}x{h})\n{url}\n")
            print(f"*** IMPROVEMENT {score0} -> {s1} — wrote {args.out} ***")
            print(url)
            return
        n_proved += sname == "INFEASIBLE"
        n_unknown += sname == "UNKNOWN"
    print(f"[sweep] done: {n_proved} proven-optimal windows, {n_unknown} UNKNOWN, "
          f"{n_skipped} skipped (state), no improvement found")


if __name__ == "__main__":
    main()

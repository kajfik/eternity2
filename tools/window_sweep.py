"""Sliding-window improve-only sweep over an E2 board via CP-SAT.

Sweeps windows that touch at least one fault cell, asking for each:
"does ANY strictly better arrangement of this window's pieces exist?" —
INFEASIBLE answers are proofs of window-local optimality; a single SAT answer
is a strict full-board improvement (on a record board: a new world record).

Window shapes (STRATEGY_460 P4): rectangles from --shapes, plus fault-shaped
windows — every 8-adjacency fault cluster dilated by Chebyshev r=1 (and r=2
while the window stays <= 45 cells). Cluster windows sit between the bare
fault clusters (INFEASIBLE in ms on strong boards) and the 45-cell rectangles
(UNKNOWN at practical budgets): they are the decidable piece-inflow tests.

Ordering (--order): hard-first (default) = most window faults first — finds
improvements fast on weak boards; easy-first = fewest win_edges (window-window
CP-SAT edges, the decidability proxy) first — banks INFEASIBLE proofs cheaply
on strong boards and shrinks the frontier before the undecidable windows eat
the budget.

Windows deliberately mix fault cells with perfect territory: that is the piece
inflow the fault-only windows cannot express. Progress is checkpointed to a
state file so the sweep can be interrupted and resumed. Each state line
records the time budget it was solved under: --retry-unknown re-attempts an
UNKNOWN window only when the current --time-per exceeds the recorded budget,
so a retry pass terminates instead of re-solving the same windows forever.
--top-unknown N is the pass-3 tier: rank the UNKNOWN windows by win_edges per
fault (ascending — most decidable per unit of fault) and give the best N a
long --time-per each (7200-14400 s: the overnight escalation).

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

CLUSTER_R2_MAX = 45  # skip r=2 cluster windows above this (undecidable scale)


def dilate(cells, r):
    """Chebyshev dilation by r, clipped to the board (same semantics as
    window_solve --auto/--dilate)."""
    out = set()
    for s in cells:
        x, y = s % W, s // W
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H:
                    out.add(ny * W + nx)
    return out


def clusters8(cells):
    """Connected components under 8-adjacency."""
    left = set(cells)
    comps = []
    while left:
        seed = left.pop()
        comp = {seed}
        stack = [seed]
        while stack:
            s = stack.pop()
            x, y = s % W, s // W
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < W and 0 <= ny < H and ny * W + nx in left:
                        n = ny * W + nx
                        left.remove(n)
                        comp.add(n)
                        stack.append(n)
        comps.append(comp)
    return comps


def win_edges_of(cells, clue_map):
    """Window-window internal edges — matches solve_window's n_win_edges
    (clue cells are removed from the window there)."""
    win = set(cells) - set(clue_map)
    n = 0
    for s in win:
        if s % W + 1 < W and s + 1 in win:
            n += 1
        if s // W + 1 < H and s + W in win:
            n += 1
    return n


def rect_windows(shapes, faults, clue_map):
    """(key, label, nf, win_edges, cells) for every WxH placement containing
    >=1 fault. Keys keep the historical '{x0},{y0},{w}x{h}' format so existing
    state files stay valid."""
    out = []
    for w, h in shapes:
        for y0 in range(0, H - h + 1):
            for x0 in range(0, W - w + 1):
                cells = {y * W + x for y in range(y0, y0 + h)
                         for x in range(x0, x0 + w)}
                nf = len(cells & faults)
                if nf:
                    out.append((f"{x0},{y0},{w}x{h}",
                                f"rect({x0},{y0}) {w}x{h}",
                                nf, win_edges_of(cells, clue_map), cells))
    return out


def cluster_windows(faults, clue_map):
    """Fault-shaped windows: each 8-adjacency fault cluster dilated r=1, and
    r=2 while <= CLUSTER_R2_MAX cells. Keys are 'clu{r}:{seed}:{ncells}' with
    seed = the cluster's smallest cell index — deterministic for a given board,
    and a distinct namespace from the rect keys."""
    out = []
    for comp in clusters8(faults):
        seed = min(comp)
        for r in (1, 2):
            cells = dilate(comp, r)
            if r == 2 and len(cells) > CLUSTER_R2_MAX:
                continue
            nf = len(cells & faults)
            out.append((f"clu{r}:{seed}:{len(cells)}",
                        f"cluster(seed={seed},r={r}) {len(cells)}c",
                        nf, win_edges_of(cells, clue_map), cells))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", required=True)
    ap.add_argument("--clues", type=int, choices=(1, 5), required=True)
    ap.add_argument("--shapes", default="8x6,6x8,9x5,5x9",
                    help="comma list of WxH window shapes")
    ap.add_argument("--clusters", type=int, default=1, choices=(0, 1),
                    help="1 = also sweep fault-cluster dilate-1/2 windows "
                         "(default), 0 = rectangles only")
    ap.add_argument("--order", choices=("hard-first", "easy-first"),
                    default="hard-first",
                    help="hard-first: most faults first (improvement hunting, "
                         "weak boards); easy-first: fewest win_edges first "
                         "(proof banking, strong boards)")
    ap.add_argument("--time-per", type=float, default=600,
                    help="CP-SAT cap per window (s)")
    ap.add_argument("--minutes", type=float, default=0,
                    help="total budget in minutes (0 = run all windows)")
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--retry-unknown", action="store_true",
                    help="re-attempt windows previously left UNKNOWN at a "
                         "smaller --time-per than the current one")
    ap.add_argument("--top-unknown", type=int, default=0, metavar="N",
                    help="pass-3 tier: run ONLY the N previously-UNKNOWN "
                         "windows with the fewest win_edges per fault, at the "
                         "current --time-per (implies --retry-unknown; "
                         "windows never attempted are not selected)")
    ap.add_argument("--out", default="../runs/candidate_sweep.txt")
    args = ap.parse_args()

    edges = load_edges(args.board)
    score0, _, be = e2lib.score_edges(edges)
    assert be == 0
    clue_map = CLUES_5 if args.clues == 5 else CLUES_1
    faults = fault_cells(edges)
    shapes = [tuple(map(int, s.split("x"))) for s in args.shapes.split(",")]
    todo = rect_windows(shapes, faults, clue_map)
    n_rect = len(todo)
    if args.clusters:
        todo += cluster_windows(faults, clue_map)
    if args.order == "easy-first":
        todo.sort(key=lambda t: (t[3], -t[2]))
    else:
        todo.sort(key=lambda t: -t[2])

    state_path = args.board + f".sweep_c{args.clues}.state"
    state = {}
    if os.path.exists(state_path):
        with open(state_path, encoding="utf-8") as f:
            state = {json.loads(l)["key"]: json.loads(l) for l in f if l.strip()}
    bhash = hashlib.md5(edges.encode()).hexdigest()[:10]

    print(f"[sweep] board score {score0}, {len(faults)} fault cells, "
          f"{len(todo)} windows to consider ({n_rect} rect + "
          f"{len(todo) - n_rect} cluster), {len(state)} already decided, "
          f"order {args.order}")
    if args.top_unknown:
        args.retry_unknown = True
        unk = [t for t in todo
               if state.get(f"{bhash}:{t[0]}", {}).get("verdict") == "UNKNOWN"]
        unk.sort(key=lambda t: t[3] / max(t[2], 1))
        todo = [t for t in unk[:args.top_unknown]
                if state[f"{bhash}:{t[0]}"].get("time", 0) < args.time_per]
        print(f"[sweep] pass-3 tier: {len(unk)} UNKNOWN windows, running the "
              f"{len(todo)} best-ranked (win_edges/fault) at {args.time_per:.0f}s")

    t_end = time.time() + args.minutes * 60 if args.minutes else None
    n_proved = n_unknown = n_skipped = 0
    for wkey, label, nf, we, cells in todo:
        key = f"{bhash}:{wkey}"
        prev = state.get(key)
        if prev and (prev["verdict"] != "UNKNOWN" or not args.retry_unknown
                     or prev.get("time", 0) >= args.time_per):
            n_skipped += 1
            continue
        if t_end and time.time() > t_end:
            print("[sweep] time budget exhausted")
            break
        print(f"[sweep] window {label} faults={nf} win_edges={we}", flush=True)
        sname, new_edges, inc, best = solve_window(
            edges, cells, clue_map, time_s=args.time_per,
            workers=args.workers, improve_only=True)
        with open(state_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"key": key, "verdict": sname, "faults": nf,
                                "we": we, "time": args.time_per}) + "\n")
        if sname in ("OPTIMAL", "FEASIBLE") and new_edges:
            s1, _, be1 = e2lib.score_edges(new_edges)
            data = e2lib.E2Data()
            e2lib.check_piece_set(data, new_edges)
            assert be1 == 0 and s1 > score0, "solver returned non-improvement?"
            url = e2lib.build_url(new_edges, name="E2Solver", data=data)
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(f"# window_sweep improvement {score0} -> {s1} "
                        f"(window {label})\n{url}\n")
            print(f"*** IMPROVEMENT {score0} -> {s1} — wrote {args.out} ***")
            print(url)
            return
        n_proved += sname == "INFEASIBLE"
        n_unknown += sname == "UNKNOWN"
    print(f"[sweep] done: {n_proved} proven-optimal windows, {n_unknown} UNKNOWN, "
          f"{n_skipped} skipped (state), no improvement found")


if __name__ == "__main__":
    main()

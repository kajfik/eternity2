"""Exact window re-solve of an E2 board via CP-SAT (OR-Tools).

Fix every cell outside a chosen window, then find the optimal assignment of
the window's piece multiset to the window cells (rotations free, rim cells
grey-out enforced, clue cells pinned). Any solution strictly better than the
incumbent is a strict full-board improvement, since all other edges are
unchanged.

This attacks the scale the C++ RegionSolver cannot reach (exhaustive to
n~16): windows of 30-70 cells, with clause learning + LP bounds. Status
OPTIMAL additionally *proves* the window admits no improvement (with its
current piece pool), telling us to grow or move the window.

Usage examples:
  python window_solve.py --board ../runs/best_c1.txt --clues 1 --auto 1 --time 600
  python window_solve.py --board ../runs/seed_5clue_461.txt --clues 5 --rect 0,11,15,15 --time 3600
  python window_solve.py --board X.txt --clues 5 --rect 0,0,6,3 --improve-only --time 300
"""

import argparse
import re
import sys
import time

import e2lib
from e2lib import rot_quad
from ortools.sat.python import cp_model

W = H = 16
NC = 256
CLUES_5 = {34: (208, 1), 45: (255, 3), 135: (139, 0), 210: (181, 3), 221: (249, 1)}
CLUES_1 = {135: (139, 0)}
UP, RIGHT, DOWN, LEFT = 0, 1, 2, 3
DX = [0, 1, 0, -1]
DY = [-1, 0, 1, 0]


def load_edges(path_or_url):
    txt = path_or_url
    if not path_or_url.startswith("http"):
        with open(path_or_url, encoding="utf-8") as f:
            txt = f.read()
    m = re.search(r"board_edges=([a-w]{1024})", txt)
    if m:
        return m.group(1)
    m = re.search(r"^([a-w]{1024})\s*$", txt, re.M)
    if m:
        return m.group(1)
    raise ValueError(f"no 1024-char edge string found in {path_or_url}")


def cell_type(s):
    """0=interior, 1=edge(rim non-corner), 2=corner."""
    x, y = s % W, s // W
    r = (x in (0, W - 1)) + (y in (0, H - 1))
    return r


def piece_type(quad):
    return quad.count("a")


def out_dirs(s):
    x, y = s % W, s // W
    dirs = []
    if y == 0:
        dirs.append(UP)
    if x == W - 1:
        dirs.append(RIGHT)
    if y == H - 1:
        dirs.append(DOWN)
    if x == 0:
        dirs.append(LEFT)
    return dirs


def allowed_rots(data, p, s):
    """Rotations of piece p valid at cell s (grey sides exactly on outward dirs)."""
    base = data.piece_quad[p]
    od = out_dirs(s)
    if piece_type(base) != len(od):
        return []
    rots, seen = [], set()
    for r in range(4):
        q = rot_quad(base, r)
        if q in seen:            # rotationally symmetric piece: dedup identical quads
            continue
        seen.add(q)
        if all(q[d] == "a" for d in od) and all(
            q[d] != "a" for d in range(4) if d not in od
        ):
            rots.append(r)
    return rots


def fault_cells(edges):
    """Cells incident to a mismatched internal edge."""
    quads = [edges[4 * i: 4 * i + 4] for i in range(NC)]
    mark = set()
    for y in range(H):
        for x in range(W):
            s = y * W + x
            if x + 1 < W:
                a, b = quads[s][RIGHT], quads[s + 1][LEFT]
                if a != b or a == "a":
                    mark.add(s); mark.add(s + 1)
            if y + 1 < H:
                a, b = quads[s][DOWN], quads[s + W][UP]
                if a != b or a == "a":
                    mark.add(s); mark.add(s + W)
    return mark


def solve_window(edges, window, clue_map, time_s=300, workers=16,
                 improve_only=False, log=False):
    """window: iterable of cell indices (clue cells auto-removed).
    Returns (status_name, new_edges, inc_gain, best_gain)."""
    data = e2lib.E2Data()
    placements = e2lib.check_piece_set(data, edges)
    for cell, (p, r) in clue_map.items():
        assert placements[cell] == (p, r), f"clue violated at {cell}"

    win = sorted(set(window) - set(clue_map))
    pool = [placements[s][0] for s in win]
    quads = [edges[4 * i: 4 * i + 4] for i in range(NC)]

    model = cp_model.CpModel()
    # x[(ci, pi, r)] : window-cell index ci gets pool piece index pi at rotation r
    x = {}
    cell_lits = [[] for _ in win]
    piece_lits = [[] for _ in pool]
    cand = {}  # (ci, pi) -> list of (r, quad)
    for ci, s in enumerate(win):
        ct = cell_type(s)
        for pi, p in enumerate(pool):
            if piece_type(data.piece_quad[p]) != ct:
                continue
            for r in allowed_rots(data, p, s):
                v = model.NewBoolVar(f"x_{s}_{p}_{r}")
                x[(ci, pi, r)] = v
                cell_lits[ci].append(v)
                piece_lits[pi].append(v)
                cand.setdefault((ci, pi), []).append(r)
    for ci in range(len(win)):
        model.AddExactlyOne(cell_lits[ci])
    for pi in range(len(pool)):
        model.AddExactlyOne(piece_lits[pi])

    win_index = {s: ci for ci, s in enumerate(win)}

    def side_expr(ci, d, k):
        """Sum of x-literals placing color k on side d of window cell ci (0/1)."""
        s = win[ci]
        lits = []
        for pi, p in enumerate(pool):
            for r in cand.get((ci, pi), []):
                if rot_quad(data.piece_quad[p], r)[d] == k:
                    lits.append(x[(ci, pi, r)])
        return lits

    obj_terms = []
    inc_gain = 0
    n_win_edges = 0
    for ci, s in enumerate(win):
        xx, yy = s % W, s // W
        for d in (RIGHT, DOWN):
            nx, ny = xx + DX[d], yy + DY[d]
            if nx < 0 or nx >= W or ny < 0 or ny >= H:
                continue
            ns = ny * W + nx
            a, b = quads[s][d], quads[ns][(d + 2) & 3]
            if ns in win_index:
                # window-window edge: per-color AND terms
                cj = win_index[ns]
                n_win_edges += 1
                if a == b and a != "a":
                    inc_gain += 1
                for k in "bcdefghijklmnopqrstuvw":
                    s1 = side_expr(ci, d, k)
                    s2 = side_expr(cj, (d + 2) & 3, k)
                    if not s1 or not s2:
                        continue
                    both = model.NewBoolVar(f"e_{s}_{ns}_{k}")
                    model.Add(sum(s1) >= both)
                    model.Add(sum(s2) >= both)
                    obj_terms.append(both)
            else:
                # window-fixed edge: neighbor color is a constant
                if a == b and a != "a":
                    inc_gain += 1
                if b != "a":
                    obj_terms.extend(side_expr(ci, d, b))
        # also edges where the fixed cell is on the left/up side of s
        for d in (LEFT, UP):
            nx, ny = xx + DX[d], yy + DY[d]
            if nx < 0 or nx >= W or ny < 0 or ny >= H:
                continue
            ns = ny * W + nx
            if ns in win_index:
                continue  # counted from the other endpoint
            a, b = quads[s][d], quads[ns][(d + 2) & 3]
            if a == b and a != "a":
                inc_gain += 1
            if b != "a":
                obj_terms.extend(side_expr(ci, d, b))

    obj = sum(obj_terms)
    if improve_only:
        model.Add(obj >= inc_gain + 1)
    else:
        model.Add(obj >= inc_gain)
        model.Maximize(obj)

    # incumbent hint
    for ci, s in enumerate(win):
        p, r = placements[s]
        pi = pool.index(p)
        for rr in cand.get((ci, pi), []):
            if rot_quad(data.piece_quad[p], rr) == rot_quad(data.piece_quad[p], r):
                model.AddHint(x[(ci, pi, rr)], 1)
                break

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_s
    solver.parameters.num_workers = workers
    solver.parameters.log_search_progress = log
    t0 = time.time()
    status = solver.Solve(model)
    dt = time.time() - t0
    sname = solver.StatusName(status)

    new_edges = None
    best_gain = None
    if status in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        best_gain = int(solver.ObjectiveValue()) if not improve_only else None
        out = list(quads)
        for (ci, pi, r), v in x.items():
            if solver.Value(v):
                out[win[ci]] = rot_quad(data.piece_quad[pool[pi]], r)
        new_edges = "".join(out)
        if improve_only:
            best_gain = sum(int(solver.Value(t)) for t in obj_terms)
    print(f"[window_solve] cells={len(win)} pool={len(pool)} vars={len(x)} "
          f"win_edges={n_win_edges} inc_gain={inc_gain} -> {sname} "
          f"best_gain={best_gain} in {dt:.1f}s")
    return sname, new_edges, inc_gain, best_gain


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", required=True, help="board file or bucas URL")
    ap.add_argument("--clues", type=int, choices=(1, 5), default=5)
    ap.add_argument("--rect", help="x0,y0,x1,y1 window rectangle (inclusive)")
    ap.add_argument("--auto", type=int, metavar="R",
                    help="window = fault cells dilated by Chebyshev radius R")
    ap.add_argument("--cells", help="explicit comma-separated cell indices")
    ap.add_argument("--board2", help="second board: window = cells where the "
                                     "two boards disagree (plateau merge)")
    ap.add_argument("--dilate", type=int, default=0,
                    help="extra Chebyshev dilation applied to --cells/--board2 windows")
    ap.add_argument("--cols", help="restrict auto window to columns lo-hi, e.g. 4-15")
    ap.add_argument("--rows", help="restrict auto window to rows lo-hi")
    ap.add_argument("--time", type=float, default=300)
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--improve-only", action="store_true",
                    help="feasibility: any solution = strict improvement; "
                         "INFEASIBLE proves window optimality")
    ap.add_argument("--log", action="store_true")
    ap.add_argument("--out", help="write improved board URL here")
    args = ap.parse_args()

    edges = load_edges(args.board)
    score0, _, be = e2lib.score_edges(edges)
    assert be == 0
    clue_map = CLUES_5 if args.clues == 5 else CLUES_1

    window = set()
    if args.rect:
        x0, y0, x1, y1 = map(int, args.rect.split(","))
        window |= {y * W + x for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)}
    if args.auto is not None:
        fc = fault_cells(edges)
        dil = set()
        for s in fc:
            xx, yy = s % W, s // W
            for dy in range(-args.auto, args.auto + 1):
                for dx in range(-args.auto, args.auto + 1):
                    nx, ny = xx + dx, yy + dy
                    if 0 <= nx < W and 0 <= ny < H:
                        dil.add(ny * W + nx)
        if args.cols:
            lo, hi = map(int, args.cols.split("-"))
            dil = {s for s in dil if lo <= s % W <= hi}
        if args.rows:
            lo, hi = map(int, args.rows.split("-"))
            dil = {s for s in dil if lo <= s // W <= hi}
        window |= dil
    if args.cells:
        window |= {int(c) for c in args.cells.split(",")}
    if args.board2:
        e2 = load_edges(args.board2)
        diff = {s for s in range(NC) if edges[4 * s: 4 * s + 4] != e2[4 * s: 4 * s + 4]}
        print(f"[diff] boards disagree on {len(diff)} cells")
        window |= diff
    if args.dilate:
        dil = set()
        for s in window:
            xx, yy = s % W, s // W
            for dy in range(-args.dilate, args.dilate + 1):
                for dx in range(-args.dilate, args.dilate + 1):
                    nx, ny = xx + dx, yy + dy
                    if 0 <= nx < W and 0 <= ny < H:
                        dil.add(ny * W + nx)
        window = dil
    if not window:
        print("empty window; pass --rect/--auto/--cells/--board2")
        sys.exit(1)

    sname, new_edges, inc, best = solve_window(
        edges, window, clue_map, time_s=args.time, workers=args.workers,
        improve_only=args.improve_only, log=args.log)

    if new_edges and new_edges != edges:
        data = e2lib.E2Data()
        score1, confl, be1 = e2lib.score_edges(new_edges)
        e2lib.check_piece_set(data, new_edges)  # raises on duplicates
        assert be1 == 0
        print(f"score {score0} -> {score1}  ({'IMPROVED +' + str(score1 - score0) if score1 > score0 else 'no gain'})")
        if score1 > score0:
            url = e2lib.build_url(new_edges, name="E2Solver", data=data)
            out = args.out or "window_improved.txt"
            with open(out, "w", encoding="utf-8") as f:
                f.write(f"# window_solve improvement {score0} -> {score1}\n{url}\n")
            print(f"WROTE {out}")
            print(url)
    else:
        print(f"score stays {score0} (status {sname})"
              + (" — window PROVEN optimal for its piece pool" if sname in ("OPTIMAL", "INFEASIBLE") else ""))


if __name__ == "__main__":
    main()

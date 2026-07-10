"""Track C: algebraic structure of the E2 color set (SOLVER_EVAL).

C1: per-color edge counts + same-piece co-occurrence.
C2: color constraint graph (nodes = colors, edge weight = #pieces bridging
    the pair via opposite sides), components / clustering / degree.
C3: zone map: P(color | ring-distance band) derived purely from piece-type
    membership counts (corner / edge / interior sides), i.e. from where a
    color CAN sit, not from any solved board. Exported as log-affinity
    weights for the solver's --zone-file.

Run: python tools/color_analysis.py [--zone-out FILE]
"""
import sys
import os
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2lib import E2Data, W, H

import numpy as np


def main():
    zone_out = None
    if "--zone-out" in sys.argv:
        zone_out = sys.argv[sys.argv.index("--zone-out") + 1]

    data = E2Data()
    # quads as 0-based color ints per piece 1..256, URDL
    quads = {p: [ord(c) - ord("a") for c in q] for p, q in data.piece_quad.items()}
    ptype = {}  # 0 interior, 1 edge, 2 corner
    for p, q in quads.items():
        g = sum(1 for c in q if c == 0)
        ptype[p] = 0 if g == 0 else (1 if g == 1 else 2)
    n_by_type = Counter(ptype.values())
    print(f"pieces: {n_by_type[0]} interior, {n_by_type[1]} edge, {n_by_type[2]} corner")

    # ---------------- C1: counts and same-piece co-occurrence
    total = Counter()          # color -> #sides overall
    by_type = {t: Counter() for t in (0, 1, 2)}
    cooc = Counter()           # (c1<=c2) -> #adjacent-side pairs on same piece
    for p, q in quads.items():
        for c in q:
            if c:
                total[c] += 1
                by_type[ptype[p]][c] += 1
        for i in range(4):     # adjacent sides of the same piece
            a, b = q[i], q[(i + 1) % 4]
            if a and b:
                cooc[(min(a, b), max(a, b))] += 1

    print("\nC1: color side counts (color: total / interior / edge / corner pieces)")
    border_colors, interior_colors = [], []
    for c in range(1, 23):
        row = (total[c], by_type[0][c], by_type[1][c], by_type[2][c])
        kind = "border" if by_type[0][c] == 0 else "inner"
        if by_type[0][c] == 0:
            border_colors.append(c)
        else:
            interior_colors.append(c)
        print(f"  color {c:2d} ({chr(ord('a')+c)}): {row[0]:3d} total | "
              f"int {row[1]:3d}  edge {row[2]:3d}  corner {row[3]:2d}  [{kind}]")
    print(f"border-frame colors (never on interior pieces): {border_colors}")
    print(f"interior colors: {interior_colors}")

    # parity check: every color must have an even number of sides
    odd = [c for c in range(1, 23) if total[c] % 2]
    print(f"odd-count colors (must pair internally -> impossible leftovers): {odd}")

    # ---------------- C2: constraint graph (opposite sides across a seam =
    # same color; a piece "bridges" colors a,b if some side has color a and
    # the opposite side color b)
    bridge = np.zeros((23, 23), dtype=int)
    for p, q in quads.items():
        for i in (0, 1):
            a, b = q[i], q[i + 2]
            if a and b:
                bridge[a][b] += 1
                if a != b:
                    bridge[b][a] += 1
    deg = [(bridge[c] > 0).sum() - (bridge[c][c] > 0) for c in range(23)]
    print("\nC2: bridge graph (piece can carry color a on one side, b opposite)")
    print("  color: weighted-degree (sum bridges) / simple-degree (of 22)")
    wdeg = {}
    for c in range(1, 23):
        wd = int(bridge[c][1:].sum())
        sd = int((bridge[c][1:] > 0).sum())
        wdeg[c] = wd
        print(f"  color {c:2d}: bridges {wd:3d}  distinct partners {sd:2d}")
    sub = (bridge[1:, 1:] > 0)
    # connected components among colors 1..22
    seen = set()
    comps = []
    for c in range(22):
        if c in seen:
            continue
        comp, stack = set(), [c]
        while stack:
            v = stack.pop()
            if v in comp:
                continue
            comp.add(v)
            stack.extend(int(u) for u in np.nonzero(sub[v])[0] if u not in comp)
        seen |= comp
        comps.append(sorted(x + 1 for x in comp))
    print(f"  connected components (colors 1-22): {len(comps)}")
    for comp in comps:
        print(f"    {comp}")
    # clustering coefficient (simple graph)
    ccs = []
    for v in range(22):
        nb = np.nonzero(sub[v])[0]
        nb = nb[nb != v]
        k = len(nb)
        if k < 2:
            continue
        links = sum(1 for i in range(k) for j in range(i + 1, k)
                    if sub[nb[i]][nb[j]])
        ccs.append(2 * links / (k * (k - 1)))
    print(f"  mean clustering coefficient: {np.mean(ccs):.3f}")
    dens = sub.sum() / (22 * 21)
    print(f"  graph density: {dens:.3f}")

    # separate view: border vs interior colors bridging
    bi = sum(bridge[a][b] for a in border_colors for b in interior_colors)
    bb = sum(bridge[a][b] for a in border_colors for b in border_colors if b >= a)
    ii = sum(bridge[a][b] for a in interior_colors for b in interior_colors if b >= a)
    print(f"  bridges border<->border {bb}, border<->interior {bi}, interior<->interior {ii}")

    # ---------------- C3: zone map from piece-type membership.
    # Ring distance d of a cell = min distance to the outer rim. The rim
    # (d=0) is corner+edge pieces; d=1 cells touch the rim's inner sides;
    # d>=2 is pure interior. Zone signal per color:
    #   band 0 (d=1): mix of edge-piece inner-side colors (must be matched
    #                 by the d=1 cells' outward sides) and interior colors
    #   bands 1..6 (d=2..7): interior-piece color distribution only
    # inner side colors of rim edge pieces (the side opposite the grey one) —
    # the only rim sides that ring-distance-1 cells ever face. Corner pieces
    # expose no inner side (2 grey + 2 lateral), so they contribute nothing.
    edge_inner = Counter()
    for p, q in quads.items():
        if ptype[p] == 1:
            i = q.index(0)
            edge_inner[q[(i + 2) % 4]] += 1
    interior_cnt = by_type[0]

    n_bands = 7
    zone = np.zeros((23, n_bands))
    tot_edge_inner = sum(edge_inner.values())
    tot_int = sum(interior_cnt.values())
    for c in range(1, 23):
        p_int = interior_cnt[c] / tot_int
        p_rim = edge_inner[c] / tot_edge_inner
        # band 0 = ring distance 1: half the cell's sides face the rim ->
        # blend; deeper bands: pure interior distribution
        zone[c][0] = 0.5 * p_rim + 0.5 * p_int
        for b in range(1, n_bands):
            zone[c][b] = p_int
    # log-affinity, normalized per band; epsilon for colors absent from a band
    eps = 1e-4
    logz = np.log(zone + eps)
    logz -= logz[1:].mean(axis=0)          # center per band over real colors
    print("\nC3: zone map (log-affinity per ring-distance band, band0 = next to rim)")
    print("  color   band0  band1+ (bands 1-6 identical by construction)")
    for c in range(1, 23):
        print(f"  {c:2d} ({chr(ord('a')+c)}): {logz[c][0]:+.2f}  {logz[c][1]:+.2f}")

    if zone_out:
        with open(zone_out, "w") as f:
            for c in range(23):
                f.write(" ".join(f"{logz[c][b]:.6f}" for b in range(n_bands)) + "\n")
        print(f"zone weights written to {zone_out}")


if __name__ == "__main__":
    main()

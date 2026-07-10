"""Track A: train a placement scorer from hunter placement logs (SOLVER_EVAL).

Input log (from solver --log-placements): one line per restart =
  depth cell:piece0:rot:wcol:ncol ...
holding the deepest DFS path of that restart. Outcome label = depth
("pieces placed" by that lineage before stalling).

Model input (8 one-hot color slots x 23 colors = 184 dims), matching the
solver's ModelWeights layout exactly:
  slots 0-3: candidate piece URDL colors (after rotation)
  slots 4-7: neighbor N,E,S,W colors (E,S always 0 = empty in row-major scan)
Labels: placements from top-tercile-depth restarts = 1, bottom tercile = 0.
Split by restart (not by placement) to avoid leakage.

Trains a logistic-linear model and a small MLP (16 hidden, relu), reports
held-out AUC/accuracy for both, writes the better one via --out.

Run: python tools/train_scorer.py LOGFILE --out model.txt
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2lib import E2Data, rot_quad

import numpy as np

NCOL, NSLOT = 23, 8
DIM = NCOL * NSLOT


def load(path):
    data = E2Data()
    quad_int = {p: [ord(c) - ord("a") for c in q] for p, q in data.piece_quad.items()}
    runs = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            depth = int(parts[0])
            recs = []
            for t in parts[1:]:
                cell, p0, r, w, n = map(int, t.split(":"))
                q = quad_int[p0 + 1]
                qr = [q[(i - r) % 4] for i in range(4)]  # QUAD[p][r]
                recs.append((cell, qr, w, n))
            runs.append((depth, recs))
    return runs


def featurize(runs, lo, hi):
    # truncate every run to its first `lo` placements so both classes cover
    # the same board positions; otherwise deep runs flood the positives with
    # placements at depths shallow runs never reach
    X, y, run_id = [], [], []
    for ri, (depth, recs) in enumerate(runs):
        if lo < depth < hi:
            continue
        label = 1 if depth >= hi else 0
        for cell, qr, w, n in recs[: int(lo)]:
            idx = [i * NCOL + qr[i] for i in range(4)]
            idx += [4 * NCOL + n, 5 * NCOL + 0, 6 * NCOL + 0, 7 * NCOL + w]
            X.append(idx)
            y.append(label)
            run_id.append(ri)
    Xi = np.array(X, dtype=np.int32)
    return Xi, np.array(y, dtype=np.float32), np.array(run_id)


def to_dense(Xi):
    X = np.zeros((len(Xi), DIM), dtype=np.float32)
    rows = np.arange(len(Xi))[:, None]
    X[rows, Xi] = 1.0
    return X


def auc(y, s):
    order = np.argsort(s)
    r = np.empty(len(s))
    r[order] = np.arange(1, len(s) + 1)
    pos = y == 1
    n1, n0 = pos.sum(), (~pos).sum()
    return (r[pos].sum() - n1 * (n1 + 1) / 2) / (n1 * n0)


def train_linear(X, y, Xv, yv, epochs=30, lr=0.5):
    w = np.zeros(DIM, dtype=np.float32)
    b = 0.0
    n = len(X)
    for ep in range(epochs):
        p = 1 / (1 + np.exp(-(X @ w + b)))
        g = (p - y) / n
        w -= lr * (X.T @ g + 1e-4 * w)
        b -= lr * g.sum()
    pv = Xv @ w + b
    return (w, b), auc(yv, pv)


def train_mlp(X, y, Xv, yv, hidden=16, epochs=40, lr=0.05, batch=8192, seed=0):
    rng = np.random.default_rng(seed)
    W1 = rng.normal(0, 0.1, (DIM, hidden)).astype(np.float32)
    b1 = np.zeros(hidden, dtype=np.float32)
    w2 = rng.normal(0, 0.1, hidden).astype(np.float32)
    b2 = 0.0
    n = len(X)
    for ep in range(epochs):
        perm = rng.permutation(n)
        for i in range(0, n, batch):
            idx = perm[i:i + batch]
            xb, yb = X[idx], y[idx]
            h = xb @ W1 + b1
            a = np.maximum(h, 0)
            p = 1 / (1 + np.exp(-(a @ w2 + b2)))
            d = (p - yb) / len(xb)
            gw2 = a.T @ d
            gb2 = d.sum()
            da = np.outer(d, w2) * (h > 0)
            gW1 = xb.T @ da
            gb1 = da.sum(axis=0)
            W1 -= lr * gW1
            b1 -= lr * gb1
            w2 -= lr * gw2
            b2 -= lr * gb2
    hv = np.maximum(Xv @ W1 + b1, 0)
    sv = hv @ w2 + b2
    return (W1, b1, w2, b2), auc(yv, sv)


def main():
    log_path = sys.argv[1]
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    runs = load(log_path)
    depths = np.array([d for d, _ in runs])
    print(f"restarts: {len(runs)}, placements: {sum(len(r) for _, r in runs)}")
    print(f"depth: mean {depths.mean():.1f}, median {np.median(depths):.0f}, "
          f"p90 {np.percentile(depths, 90):.0f}, max {depths.max()}")
    lo, hi = np.percentile(depths, 33), np.percentile(depths, 67)
    print(f"label terciles: neg depth<={lo:.0f}, pos depth>={hi:.0f}")

    Xi, y, run_id = featurize(runs, lo, hi)
    print(f"labeled placements: {len(y)} ({y.mean()*100:.1f}% positive)")
    # split by restart
    rng = np.random.default_rng(42)
    rids = np.unique(run_id)
    rng.shuffle(rids)
    val_ids = set(rids[: len(rids) // 5].tolist())
    vm = np.isin(run_id, list(val_ids))
    X = to_dense(Xi)
    Xt, yt, Xv, yv = X[~vm], y[~vm], X[vm], y[vm]
    print(f"train {len(yt)}, val {len(yv)}")

    (wl, bl), auc_lin = train_linear(Xt, yt, Xv, yv)
    print(f"linear:  val AUC {auc_lin:.4f}")
    (W1, b1, w2, b2), auc_mlp = train_mlp(Xt, yt, Xv, yv)
    print(f"mlp(16): val AUC {auc_mlp:.4f}")

    if out:
        if auc_mlp > auc_lin + 0.002:
            with open(out, "w") as f:
                f.write(f"MLP {W1.shape[1]}\n")
                np.savetxt(f, W1.T.reshape(-1)[None], fmt="%.6f")
                np.savetxt(f, b1[None], fmt="%.6f")
                np.savetxt(f, w2[None], fmt="%.6f")
                f.write(f"{b2:.6f}\n")
            print(f"wrote MLP -> {out}")
        else:
            with open(out, "w") as f:
                f.write("LINEAR\n")
                np.savetxt(f, wl[None], fmt="%.6f")
                f.write(f"{bl:.6f}\n")
            print(f"wrote LINEAR -> {out}")


if __name__ == "__main__":
    main()

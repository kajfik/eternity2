#!/usr/bin/env python3
"""ACTUARY: joint fill-order + slip-schedule optimization (raphael-anjou
vol-216 rebuild of Verhaard's order+slip Markov model, groups.io msg 6423).

Pipeline:
  1. Capture per-(depth, slips-spent) fit statistics with the solver's
     --fitstats FILE flag (one capture per fill order: row-major, and
     --tail-cols N comb variants -- their per-depth rates differ, which is
     the whole point of joint optimization). Several captures with different
     slip schedules widen (d, s) coverage; pass them all.
  2. Model the hunter DFS as a branching process over states (d, s):
       lam_e(d,s) = exact-fit children per node expansion
       lam_h(d,s) = one-mismatch children per expansion when the schedule
                    has headroom
     Where a capture measured a cell (enough visits, headroom for the half
     rate), its realized child rate is used directly -- this bakes in the
     color-deficit pruning, which is a property of (depth, pool damage) and
     carries across schedules with the same total budget T. Unmeasured cells
     factorize as availability(d,s) x rho(depth, remaining budget): the
     acceptance ratio rho is ~1 with comfortable remaining budget and
     craters near exhaustion (measured, bucketed, nearest-filled).
  3. DP for a candidate cumulative gate schedule caps[d] (monotone, from T
     gate depths g_1<=...<=g_T; slip k unlocks at scan index g_k):
       C[d][s] = lam_e*C[d+1][s] + [s+1<=caps[d]]*lam_h*C[d+1][s+1]
       N[d][s] = 1 + lam_e*N[d+1][s] + [s+1<=caps[d]]*lam_h*N[d+1][s+1]
     with C[dstar][*] = 1 (arrival), N[dstar][*] = 0.
     Objective = C[0,0]/N[0,0] = expected deep arrivals per search node.
  4. Simulated annealing over the gate depths; emits a --slip-gates string.

Validation: the forward pass under the measurement schedule must reproduce
the measured visits/restart profile (printed as "forward check"); a big
mismatch means the Markov (d,s) state is missing something for that data.

Usage:
  python tools/actuary.py --stats CAP1 [CAP2 ...] [--T 20] [--dstar 0]
      [--iters 40000] [--chains 3] [--seed 1] [--out gates.txt]
  --dstar 0 means full completion (d* = n_scan). All captures passed in one
  invocation must share the same fill order (same tail_cols).
"""

import argparse
import math
import random
import sys

import numpy as np

FS_D, FS_S = 200, 64
MINV = 300     # realized-rate trust threshold (visits)
MINSMP = 60    # availability trust threshold (samples)
DBKT = 8       # rho depth-bucket size


class Capture:
    def __init__(self, path):
        keys = ["visits", "samples", "avail_e", "avail_h", "child_e", "child_h"]
        self.c = {k: np.zeros((FS_D, FS_S)) for k in keys}
        self.surv = np.zeros(FS_D + 1)
        self.restarts = 0
        self.caps = None
        self.meta = {}
        with open(path) as f:
            for line in f:
                t = line.split()
                if not t:
                    continue
                if t[0] == "#":
                    self.meta = dict(kv.split("=") for kv in t[2:] if "=" in kv)
                elif t[0] == "CAPS":
                    self.T = int(t[1])
                    self.caps = np.array([int(x) for x in t[2:]])
                elif t[0] == "R":
                    self.restarts = int(t[1])
                elif t[0] == "S":
                    self.surv[int(t[1])] = int(t[2])
                elif t[0] == "D":
                    d, s = int(t[1]), int(t[2])
                    for k, v in zip(keys, t[3:9]):
                        self.c[k][d, s] = float(v)
        self.n_scan = int(self.meta["n_scan"])
        self.tail_cols = self.meta.get("tail_cols", "0")


def estimate_rates(caps_list, T):
    """lam_e[d,s], lam_h[d,s] for the anneal budget T, s in [0,T]."""
    n = caps_list[0].n_scan
    nr = max(cp.T for cp in caps_list) + 1

    # --- acceptance rho(d_bucket, r) from all captures ---
    nb = (n + DBKT - 1) // DBKT
    wE = np.zeros((nb, nr + 1)); vE = np.zeros((nb, nr + 1))
    wH = np.zeros((nb, nr + 1)); vH = np.zeros((nb, nr + 1))
    for cp in caps_list:
        c = cp.c
        for d in range(n):
            b = d // DBKT
            for s in range(min(cp.T + 1, FS_S)):
                vis, smp = c["visits"][d, s], c["samples"][d, s]
                if vis < MINV or smp < MINSMP:
                    continue
                ae = c["avail_e"][d, s] / smp
                ah = c["avail_h"][d, s] / smp
                rE = min(cp.T - s, nr)
                if ae > 0.02 and rE >= 0:
                    wE[b, rE] += vis
                    vE[b, rE] += vis * min(c["child_e"][d, s] / vis / ae, 1.5)
                rH = cp.T - s - 1
                if ah > 0.02 and 0 <= rH and s + 1 <= cp.caps[d]:
                    wH[b, min(rH, nr)] += vis
                    vH[b, min(rH, nr)] += vis * min(c["child_h"][d, s] / vis / ah, 1.5)

    def fill(w, v):
        rho = np.full(w.shape, np.nan)
        m = w > 0
        rho[m] = v[m] / w[m]
        for b in range(w.shape[0]):        # nearest r within bucket
            row = rho[b]
            idx = np.where(~np.isnan(row))[0]
            if len(idx):
                for r in range(len(row)):
                    row[r] = row[idx[np.abs(idx - r).argmin()]]
        for b in range(w.shape[0]):        # nearest measured bucket
            if np.isnan(rho[b]).all():
                for off in range(1, w.shape[0]):
                    for b2 in (b - off, b + off):
                        if 0 <= b2 < w.shape[0] and not np.isnan(rho[b2]).all():
                            rho[b] = rho[b2]
                            break
                    else:
                        continue
                    break
        return np.nan_to_num(rho, nan=1.0)

    rhoE, rhoH = fill(wE, vE), fill(wH, vH)

    # --- availability marginals (visit-weighted across captures) ---
    ae_m = np.zeros(n); ah_m = np.zeros(n); wm = np.zeros(n)
    for cp in caps_list:
        c = cp.c
        smp = c["samples"][:n].sum(axis=1)
        w = np.maximum(smp, 1e-9)
        ae_m += c["avail_e"][:n].sum(axis=1)
        ah_m += c["avail_h"][:n].sum(axis=1)
        wm += smp
    good = wm >= MINSMP
    ae_m[good] /= wm[good]; ah_m[good] /= wm[good]
    last = int(np.max(np.nonzero(good))) if good.any() else 0
    for d in range(n):                     # decay unmeasured tail w/ pieces left
        if not good[d]:
            src = last if d > last else int(np.min(np.nonzero(good)))
            ae_m[d] = ae_m[src] * (n - d) / max(1, n - src)
            ah_m[d] = ah_m[src] * (n - d) / max(1, n - src)

    # --- per-cell lambda ---
    lam_e = np.zeros((n, T + 1)); lam_h = np.zeros((n, T + 1))
    for d in range(n):
        b = d // DBKT
        for s in range(T + 1):
            # exact: realized from same-T captures first (deficit pruning at
            # remaining budget r matches), else availability x rhoE
            veq = sum(cp.c["visits"][d, s] for cp in caps_list if cp.T == T)
            ceq = sum(cp.c["child_e"][d, s] for cp in caps_list if cp.T == T)
            if veq >= MINV:
                lam_e[d, s] = ceq / veq
            else:
                smp = sum(cp.c["samples"][d, s] for cp in caps_list)
                av = sum(cp.c["avail_e"][d, s] for cp in caps_list)
                ae = av / smp if smp >= MINSMP else ae_m[d]
                lam_e[d, s] = ae * rhoE[b, min(max(T - s, 0), nr)]
            # half: realized only from same-T captures with headroom
            vh = sum(cp.c["visits"][d, s] for cp in caps_list
                     if cp.T == T and s + 1 <= cp.caps[d])
            ch = sum(cp.c["child_h"][d, s] for cp in caps_list
                     if cp.T == T and s + 1 <= cp.caps[d])
            r = T - s - 1
            if r < 0:
                lam_h[d, s] = 0.0
            elif vh >= MINV:
                lam_h[d, s] = ch / vh
            else:
                smp = sum(cp.c["samples"][d, s] for cp in caps_list)
                av = sum(cp.c["avail_h"][d, s] for cp in caps_list)
                ah = av / smp if smp >= MINSMP else ah_m[d]
                lam_h[d, s] = ah * rhoH[b, min(r, nr)]
    return lam_e, lam_h


def gates_to_caps(gates, n_scan):
    caps = np.zeros(n_scan, dtype=np.int32)
    for g in gates:
        caps[max(0, min(g, n_scan - 1)):] += 1
    return caps


def caps_to_gates(caps):
    gates, prev = [], 0
    for d, cv in enumerate(caps):
        gates += [d] * (cv - prev)
        prev = cv
    return gates


def sqrt_caps(n_scan, target, span_frac, max_score=480):
    """Replicates the C++ sqrt schedule exactly (build_slip_caps)."""
    T = max_score - target
    span = min(n_scan, max(T, int(span_frac * n_scan)))
    d0 = n_scan - span
    rise = max(1, span - max(4, span // 10))
    caps = np.zeros(n_scan, dtype=np.int32)
    for k in range(1, T + 1):
        dk = d0 + int(round(rise * math.sqrt((k - 1) / T)))
        caps[min(dk, n_scan - 1):] += 1
    return caps


def dp(caps, lam_e, lam_h, T, dstar, node_cap=40e6):
    """Returns (arrivals_per_root, nodes_per_root, arrivals_per_node).

    node_cap models the per-restart DFS node budget: a subtree whose expected
    size exceeds the cap is explored only up to the cap, keeping its
    arrivals-per-node density (DFS truncation takes a contiguous prefix of
    sibling subtrees at the same state, so arrivals scale ~proportionally).
    Without this, supercritical bloom regions (lam_e+lam_h > 1) make the
    unbounded expectation diverge and the anneal exploits fictional subtrees.
    """
    C = np.ones(T + 1)
    N = np.zeros(T + 1)
    srange = np.arange(T + 1)
    for d in range(dstar - 1, -1, -1):
        allow = (srange + 1) <= caps[d]
        C1 = np.empty(T + 1); C1[:-1] = C[1:]; C1[-1] = 0.0
        N1 = np.empty(T + 1); N1[:-1] = N[1:]; N1[-1] = 0.0
        lh = allow * lam_h[d]
        C = lam_e[d] * C + lh * C1
        N = 1.0 + lam_e[d] * N + lh * N1
        over = N > node_cap
        if over.any():
            scale = node_cap / N[over]
            C[over] *= scale
            N[over] = node_cap
    return (C[0], N[0], C[0] / N[0]) if N[0] > 0 else (0.0, 1.0, 0.0)


def forward(caps, lam_e, lam_h, T, n_scan):
    """Expected visits/restart per depth (and final arrival mass)."""
    v = np.zeros(T + 1); v[0] = 1.0
    pred = np.zeros(n_scan + 1); pred[0] = 1.0
    srange = np.arange(T + 1)
    for d in range(n_scan):
        allow = (srange + 1) <= caps[d]
        nv = lam_e[d] * v
        nv[1:] += (allow * lam_h[d] * v)[:-1]
        v = nv
        pred[d + 1] = v.sum()
    return pred


def anneal(gates0, lam_e, lam_h, T, dstar, n_scan, iters, seed, node_cap=40e6,
           t0=0.5, t1=0.005):
    rng = random.Random(seed)
    cur = sorted(gates0)
    fcur = dp(gates_to_caps(cur, n_scan), lam_e, lam_h, T, dstar, node_cap)[2]
    best, fbest = list(cur), fcur
    lcur = math.log10(max(fcur, 1e-300))
    for it in range(iters):
        temp = t0 * (t1 / t0) ** (it / max(1, iters - 1))
        cand = list(cur)
        if rng.random() < 0.2:  # block move: shift a suffix of gates
            k = rng.randrange(T)
            delta = rng.choice([-16, -8, -4, -2, 2, 4, 8, 16])
            for j in range(k, T):
                cand[j] = max(0, min(n_scan - 1, cand[j] + delta))
        else:
            k = rng.randrange(T)
            delta = rng.choice([-24, -16, -8, -4, -2, -1, 1, 2, 4, 8, 16, 24])
            cand[k] = max(0, min(n_scan - 1, cand[k] + delta))
        cand.sort()
        f = dp(gates_to_caps(cand, n_scan), lam_e, lam_h, T, dstar, node_cap)[2]
        lf = math.log10(max(f, 1e-300))
        if lf >= lcur or rng.random() < math.exp((lf - lcur) / max(temp, 1e-9)):
            cur, lcur = cand, lf
            if f > fbest:
                best, fbest = list(cand), f
    return best, fbest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stats", nargs="+", required=True)
    ap.add_argument("--T", type=int, default=20, help="total slips (480-target)")
    ap.add_argument("--dstar", type=int, default=0, help="arrival depth; 0 = n_scan")
    ap.add_argument("--iters", type=int, default=40000)
    ap.add_argument("--chains", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--span", type=float, default=0.30, help="sqrt baseline span")
    ap.add_argument("--node-cap", type=float, default=40e6,
                    help="per-restart DFS node budget (solver --node-cap)")
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    caps_list = [Capture(p) for p in a.stats]
    if len({cp.tail_cols for cp in caps_list}) > 1:
        sys.exit("ERROR: captures mix fill orders (tail_cols differs)")
    n_scan = caps_list[0].n_scan
    T = a.T
    dstar = a.dstar if a.dstar > 0 else n_scan
    lam_e, lam_h = estimate_rates(caps_list, T)
    print(f"order: tail_cols={caps_list[0].tail_cols} n_scan={n_scan} "
          f"restarts={sum(cp.restarts for cp in caps_list)}")

    base_caps = sqrt_caps(n_scan, 480 - T, a.span)
    fb = dp(base_caps, lam_e, lam_h, T, dstar, a.node_cap)
    print(f"baseline sqrt-{480 - T} span={a.span}: arrivals/root={fb[0]:.3e} "
          f"nodes/root={fb[1]:.3e} arrivals/node={fb[2]:.3e}")
    # forward check against the first capture's own schedule
    cp0 = caps_list[0]
    lam_e0, lam_h0 = (lam_e, lam_h) if cp0.T == T else estimate_rates(caps_list, cp0.T)
    pred = forward(cp0.caps, lam_e0, lam_h0, cp0.T, n_scan)
    print("forward check (capture 0 schedule) d: predicted vs measured visits/restart")
    for d in [40, 80, 120, 140, 150, 160, 170, 175, 180, 185]:
        if d < n_scan and cp0.restarts > 0:
            meas = cp0.c["visits"][d].sum() / cp0.restarts
            print(f"  d={d:3d}  pred={pred[d]:11.4e}  meas={meas:11.4e}")

    inits = [caps_to_gates(base_caps)]
    inits.append(sorted(min(n_scan - 1, int(n_scan * (0.75 + 0.25 * k / T)))
                        for k in range(T)))
    inits.append(sorted(min(n_scan - 1, int(n_scan * (0.5 + 0.5 * k / T)))
                        for k in range(T)))
    best, fbest = None, -1.0
    for ch in range(a.chains):
        g0 = inits[ch % len(inits)]
        g, f = anneal(g0, lam_e, lam_h, T, dstar, n_scan, a.iters, a.seed + ch,
                      a.node_cap)
        print(f"chain {ch}: arrivals/node={f:.3e} gates={','.join(map(str, g))}")
        if f > fbest:
            best, fbest = g, f
    bc = gates_to_caps(best, n_scan)
    fx = dp(bc, lam_e, lam_h, T, dstar, a.node_cap)
    print(f"\nANNEALED: arrivals/root={fx[0]:.3e} nodes/root={fx[1]:.3e} "
          f"arrivals/node={fx[2]:.3e}  (x{fx[2] / max(fb[2], 1e-300):.2f} vs sqrt)")
    gate_str = ",".join(map(str, best))
    print(f"--slip-gates {gate_str}")
    if a.out:
        with open(a.out, "w") as f:
            f.write(gate_str + "\n")


if __name__ == "__main__":
    main()

r"""Phase-0 (STRATEGY_460 P1) merge-ladder driver — resumable compute campaign.

Runs window_solve.py --improve-only probes over the 12 cross-sub-lineage
basin-B 455 pairs at an escalating time ladder (600 s -> 1800 s -> 14400 s),
plus optional dilate-1 fault-cluster window probes on the two basin-A boards.
One CP-SAT solve at a time, --workers 16, machine otherwise idle.

State is persisted to docs/phase0_state.json after EVERY probe (atomic
write), so the campaign can be killed at any point and resumed by simply
re-running this script; at most the in-flight probe is lost. Human-readable
log appends to docs/phase0_results.md; raw solver output lands in
runs/phase0_logs/.

Usage (from repo root):
  python tools\phase0_ladder.py            # tiers 1+2, then stop
  python tools\phase0_ladder.py --tier3    # also run tier 3 (3 smallest UNKNOWNs @ 4 h)
  python tools\phase0_ladder.py --clusters # also run dilate-1 cluster probes
  python tools\phase0_ladder.py --status   # print state summary and exit

Any SAT: e2lib-verified, saved as runs/candidate_merge_<score>_<hash8>.txt
(md5-prefix naming, same as plateau_merge.py), bucas URL printed, exit 42.
"""

import argparse
import datetime
import hashlib
import itertools
import json
import os
import re
import subprocess
import sys
import time

import e2lib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE_PATH = os.path.join(ROOT, "docs", "phase0_state.json")
RESULTS_PATH = os.path.join(ROOT, "docs", "phase0_results.md")
LOG_DIR = os.path.join(ROOT, "runs", "phase0_logs")
WS = os.path.join(ROOT, "tools", "window_solve.py")

W = H = 16
NC = 256
CLUES_5 = {34: (208, 1), 45: (255, 3), 135: (139, 0), 210: (181, 3), 221: (249, 1)}

BOARDS = {
    "45bbcff6": r"runs\candidate_merge_455_45bbcff6.txt",
    "a775bd8f": r"runs\candidate_merge_455_a775bd8f.txt",
    "35985248": r"runs\archive_455_basinB\candidate_merge_455_35985248.txt",
    "4279accd": r"runs\archive_455_basinB\candidate_merge_455_4279accd.txt",
    "addb1f31": r"runs\archive_455_basinB\candidate_merge_455_addb1f31.txt",
    "b3cd2796": r"runs\archive_455_basinB\candidate_merge_455_b3cd2796.txt",
    "b9b532fe": r"runs\archive_455_basinB\candidate_merge_455_b9b532fe.txt",
    "cbdf0052": r"runs\archive_455_basinB\candidate_merge_455_cbdf0052.txt",
}
BASIN_A = {"45bbcff6", "a775bd8f"}
TIER1, TIER2, TIER3 = 600, 1800, 14400


def now():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def load_edges(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        txt = f.read()
    m = re.search(r"board_edges=([a-w]{1024})", txt) or \
        re.search(r"^([a-w]{1024})\s*$", txt, re.M)
    if not m:
        raise ValueError(f"no edges in {path}")
    return m.group(1)


def load_state():
    if os.path.exists(STATE_PATH):
        with open(STATE_PATH, encoding="utf-8") as f:
            return json.load(f)
    return {"created": now(), "boards": {}, "pairs": {}, "clusters": {}, "sat": None}


def save_state(st):
    tmp = STATE_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(st, f, indent=1)
    os.replace(tmp, STATE_PATH)


def append_results(line):
    with open(RESULTS_PATH, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def ensure_results_header():
    if not os.path.exists(RESULTS_PATH):
        with open(RESULTS_PATH, "w", encoding="utf-8") as f:
            f.write(
                "# Phase 0 (STRATEGY_460 P1) — 455 merge-ladder + cluster-window results\n\n"
                "Campaign started 2026-07-13. Driver: `tools/phase0_ladder.py` "
                "(resumable; state in `docs/phase0_state.json`).\n\n"
                "8 distinct 455/480 5-clue boards, e2lib-verified (455, 0 border errors, "
                "clean multiset, all 5 clues). Basin A = {45bbcff6 (= runs_scratch best_c5), "
                "a775bd8f}; basin B = six boards archived in `runs/archive_455_basinB/` "
                "(byte-verified against git HEAD). All probes: "
                "`window_solve.py --clues 5 --improve-only --workers 16`, sequential, machine idle. "
                "INFEASIBLE = window proven optimal for its piece pool; any SAT = 456.\n\n"
                "| probe | window | cells | time(s) | verdict | wall(s) | win_edges | inc_gain | when |\n"
                "|---|---|---|---|---|---|---|---|---|\n")


def machine_idle_or_die():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq solver.exe"],
                         capture_output=True, text=True).stdout
    if "solver.exe" in out:
        print("ABORT: solver.exe is running — machine must be idle for probes.")
        sys.exit(3)


def verify_boards(st):
    data = e2lib.E2Data()
    for name, path in BOARDS.items():
        e = load_edges(path)
        score, _, be = e2lib.score_edges(e)
        placements = e2lib.check_piece_set(data, e)  # raises on multiset error
        clues_ok = all(placements[c] == pr for c, pr in CLUES_5.items())
        if not (score == 455 and be == 0 and clues_ok):
            print(f"ABORT: board {name} failed verification "
                  f"(score={score} be={be} clues={clues_ok})")
            sys.exit(2)
        st["boards"][name] = {"path": path, "score": score, "verified": now()}
    return {n: load_edges(p) for n, p in BOARDS.items()}


def init_pairs(st, edges):
    for a, b in itertools.combinations(sorted(BOARDS), 2):
        key = f"{a}x{b}"
        if key in st["pairs"]:
            continue
        ea, eb = edges[a], edges[b]
        diff = sum(1 for s in range(NC) if ea[4*s:4*s+4] != eb[4*s:4*s+4])
        skip = None
        if diff < 5:
            skip = "diff<5 trivially INFEASIBLE"
        elif diff > 45:
            skip = "cross-basin (diff>45), merge void"
        st["pairs"][key] = {"a": a, "b": b, "diff": diff, "skip": skip, "results": {}}
    save_state(st)


def dilate1(cells):
    out = set()
    for s in cells:
        x, y = s % W, s // W
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if 0 <= x + dx < W and 0 <= y + dy < H:
                    out.add((y + dy) * W + (x + dx))
    return out


def model_signature(board_name, window_cells):
    """Two probes with equal signature are the same CP-SAT model: same window
    cells, same placements everywhere outside (hence same fixed boundary), and
    same window piece pool. The incumbent hint is the only difference, and
    improve-only ignores it (the incumbent violates obj >= inc+1)."""
    e = load_edges(BOARDS[board_name])
    win = sorted(set(window_cells) - set(CLUES_5))
    data = e2lib.E2Data()
    placements = e2lib.check_piece_set(data, e)
    pool = sorted(placements[s][0] for s in win)
    wset = set(win)
    outside = "".join("...." if s in wset else e[4*s:4*s+4] for s in range(NC))
    blob = ",".join(map(str, win)) + "|" + outside + "|" + ",".join(map(str, pool))
    return hashlib.md5(blob.encode()).hexdigest()[:12]


def fault_cells(edges):
    quads = [edges[4*i:4*i+4] for i in range(NC)]
    mark = set()
    for y in range(H):
        for x in range(W):
            s = y * W + x
            if x + 1 < W:
                a, b = quads[s][1], quads[s+1][3]
                if a != b or a == "a":
                    mark.add(s); mark.add(s+1)
            if y + 1 < H:
                a, b = quads[s][2], quads[s+W][0]
                if a != b or a == "a":
                    mark.add(s); mark.add(s+W)
    return mark


def clusters_8adj(cells):
    cells = set(cells)
    out = []
    while cells:
        seed = cells.pop()
        comp, frontier = {seed}, [seed]
        while frontier:
            s = frontier.pop()
            x, y = s % W, s // W
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    n = (y+dy) * W + (x+dx)
                    if 0 <= x+dx < W and 0 <= y+dy < H and n in cells:
                        cells.remove(n)
                        comp.add(n)
                        frontier.append(n)
        out.append(sorted(comp))
    out.sort(key=len, reverse=True)
    return out


def init_clusters(st, edges, cluster_time):
    for name in sorted(BASIN_A):
        fc = fault_cells(edges[name])
        for i, comp in enumerate(clusters_8adj(fc)):
            key = f"cluster_{name}_{i}"
            if key in st["clusters"]:
                continue
            st["clusters"][key] = {
                "board": name, "cells": comp, "n_cells": len(comp),
                "dilate": 1, "time": cluster_time, "results": {}}
    save_state(st)


def parse_output(out):
    status, wall, win_edges, inc_gain = None, None, None, None
    m = re.search(r"win_edges=(\d+) inc_gain=(\d+) -> (\w+) best_gain=\S+ in ([\d.]+)s", out)
    if m:
        win_edges, inc_gain, status, wall = int(m.group(1)), int(m.group(2)), m.group(3), float(m.group(4))
    return status, wall, win_edges, inc_gain


def handle_sat(st, out_file, probe_desc):
    with open(out_file, encoding="utf-8") as f:
        txt = f.read()
    m = re.search(r"board_edges=([a-w]{1024})", txt)
    e = m.group(1)
    data = e2lib.E2Data()
    score, _, be = e2lib.score_edges(e)
    placements = e2lib.check_piece_set(data, e)
    clues_ok = all(placements[c] == pr for c, pr in CLUES_5.items())
    assert be == 0 and clues_ok and score > 455, \
        f"SAT verification FAILED: score={score} be={be} clues={clues_ok}"
    h8 = hashlib.md5(e.encode()).hexdigest()[:8]
    url = e2lib.build_url(e, name="E2Solver", data=data)
    dest = os.path.join(ROOT, "runs", f"candidate_merge_{score}_{h8}.txt")
    with open(dest, "w", encoding="utf-8") as f:
        f.write(f"# phase0_ladder improvement 455 -> {score} ({probe_desc})\n{url}\n")
    st["sat"] = {"score": score, "hash8": h8, "probe": probe_desc,
                 "file": dest, "url": url, "when": now()}
    save_state(st)
    append_results(f"\n**SAT — {score}/480 found by {probe_desc}, saved "
                   f"`runs/candidate_merge_{score}_{h8}.txt` at {now()}**\n\n{url}\n")
    print("=" * 70)
    print(f"*** SAT: {score}/480 (e2lib-verified: 0 border errors, clean multiset, "
          f"all 5 clues) ***")
    print(f"probe: {probe_desc}")
    print(f"saved: {dest}")
    print(url)
    print("=" * 70)
    sys.exit(42)


def run_probe(st, probe_id, desc, args_extra, time_s, cells_n):
    machine_idle_or_die()
    os.makedirs(LOG_DIR, exist_ok=True)
    log_path = os.path.join(LOG_DIR, f"{probe_id}_{time_s}.log")
    out_file = os.path.join(LOG_DIR, f"{probe_id}_{time_s}_improved.txt")
    cmd = [sys.executable, WS, "--clues", "5", "--improve-only",
           "--time", str(time_s), "--workers", "16", "--out", out_file] + args_extra
    print(f"[{now()}] {desc} @ {time_s}s ...", flush=True)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                           cwd=ROOT, timeout=time_s + 1800)
        out = r.stdout + r.stderr
    except subprocess.TimeoutExpired as ex:
        out = ((ex.stdout or "") if isinstance(ex.stdout, str) else "") + " HARD-TIMEOUT"
    wall = time.time() - t0
    with open(log_path, "w", encoding="utf-8") as f:
        f.write(out)
    status, solve_s, win_edges, inc_gain = parse_output(out)
    if status is None:
        status = "ERROR"
    result = {"status": status, "wall": round(wall, 1), "solve_s": solve_s,
              "win_edges": win_edges, "inc_gain": inc_gain, "when": now()}
    print(f"[{now()}]   -> {status} (wall {wall:.0f}s, win_edges={win_edges}, "
          f"inc_gain={inc_gain})", flush=True)
    append_results(f"| {desc} | {'pair-diff' if 'x' in probe_id else 'fault-cluster dil-1'} "
                   f"| {cells_n} | {time_s} | {status} | {round(wall,1)} "
                   f"| {win_edges} | {inc_gain} | {now()} |")
    sat = os.path.exists(out_file) and "WROTE" in out
    return result, sat, out_file


def pair_result(st, key, time_s):
    return st["pairs"][key]["results"].get(str(time_s))


def run_pair(st, key, time_s):
    p = st["pairs"][key]
    a, b = p["a"], p["b"]
    desc = f"{a}×{b} (diff {p['diff']})"
    extra = ["--board", BOARDS[a], "--board2", BOARDS[b]]
    result, sat, out_file = run_probe(st, key, desc, extra, time_s, p["diff"])
    p["results"][str(time_s)] = result
    save_state(st)
    if sat:
        handle_sat(st, out_file, f"pair merge {a}×{b}, diff {p['diff']}, {time_s}s")
    return result


def run_cluster(st, key):
    c = st["clusters"][key]
    time_s = c["time"]
    if str(time_s) in c["results"]:
        return c["results"][str(time_s)]
    desc = f"{c['board']} cluster{key[-1]} ({c['n_cells']}c dil-1)"
    extra = ["--board", BOARDS[c["board"]],
             "--cells", ",".join(map(str, c["cells"])), "--dilate", "1"]
    result, sat, out_file = run_probe(st, key, desc, extra, time_s, c["n_cells"])
    c["results"][str(time_s)] = result
    save_state(st)
    if sat:
        handle_sat(st, out_file, f"dilate-1 fault-cluster window on {c['board']} "
                                 f"({c['n_cells']} cluster cells), {time_s}s")
    return result


def eligible_pairs(st):
    return sorted((k for k, p in st["pairs"].items() if not p["skip"]),
                  key=lambda k: st["pairs"][k]["diff"])


def print_status(st):
    print(f"state: {STATE_PATH}")
    if st["sat"]:
        print(f"SAT FOUND: {st['sat']}")
    for k in eligible_pairs(st):
        p = st["pairs"][k]
        res = ", ".join(f"{t}s:{r['status']}" for t, r in sorted(p["results"].items(), key=lambda x: int(x[0])))
        print(f"  pair {k} diff={p['diff']:2d}  [{res or 'pending'}]")
    for k, c in sorted(st["clusters"].items()):
        res = ", ".join(f"{t}s:{r['status']}" for t, r in c["results"].items())
        print(f"  {k} n={c['n_cells']}  [{res or 'pending'}]")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier3", action="store_true", help="run tier 3 (3 smallest UNKNOWNs @ 14400 s)")
    ap.add_argument("--clusters", action="store_true", help="run dilate-1 fault-cluster probes on basin A")
    ap.add_argument("--cluster-time", type=int, default=14400)
    ap.add_argument("--skip-tiers", action="store_true", help="with --clusters: skip pair tiers")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()

    st = load_state()
    if args.status:
        print_status(st)
        return
    if st["sat"]:
        print(f"SAT already found — stop. {st['sat']['url']}")
        return

    ensure_results_header()
    edges = verify_boards(st)
    init_pairs(st, edges)
    if args.clusters:
        init_clusters(st, edges, args.cluster_time)
    save_state(st)

    if not args.skip_tiers:
        # Tier 1: every eligible pair @ 600 s (ascending diff)
        for k in eligible_pairs(st):
            if not pair_result(st, k, TIER1):
                run_pair(st, k, TIER1)
        # Tier 2: tier-1 UNKNOWNs @ 1800 s
        for k in eligible_pairs(st):
            r1 = pair_result(st, k, TIER1)
            if r1 and r1["status"] == "UNKNOWN" and not pair_result(st, k, TIER2):
                run_pair(st, k, TIER2)
        # Tier 3 (gated): pairs still UNKNOWN after tier 2, deduped by CP-SAT
        # model signature (identical window+outside+pool => one probe decides
        # the whole group), up to 3 distinct models, smallest diff first
        t3 = [k for k in eligible_pairs(st)
              if (pair_result(st, k, TIER2) or {}).get("status") == "UNKNOWN"]
        groups = {}
        for k in t3:
            p = st["pairs"][k]
            ea, eb = edges[p["a"]], edges[p["b"]]
            win = [s for s in range(NC) if ea[4*s:4*s+4] != eb[4*s:4*s+4]]
            groups.setdefault(model_signature(p["a"], win), []).append(k)
        gitems = list(groups.items())[:3]
        if args.tier3:
            for sig, members in gitems:
                done = next((m for m in members if pair_result(st, m, TIER3)), None)
                if done:
                    res = pair_result(st, done, TIER3)
                else:
                    done = members[0]
                    res = run_pair(st, done, TIER3)
                copies = [m for m in members if m != done and not pair_result(st, m, TIER3)]
                for m in copies:
                    st["pairs"][m]["results"][str(TIER3)] = {**res, "via": done}
                if copies:
                    save_state(st)
                    append_results(
                        f"| {', '.join(copies)} | pair-diff (same model as {done}) "
                        f"| {st['pairs'][done]['diff']} | {TIER3} | {res['status']} "
                        f"| via {done} | {res.get('win_edges')} | {res.get('inc_gain')} | {now()} |")
        elif gitems:
            print(f"[{now()}] tier 3 candidates (need --tier3, {len(gitems)} distinct "
                  f"model(s), ~{len(gitems)*4}h): "
                  + "; ".join(f"{'/'.join(ms)}" for _, ms in gitems))

    if args.clusters:
        cgroups = {}
        for k in sorted(st["clusters"]):
            c = st["clusters"][k]
            sig = model_signature(c["board"], dilate1(c["cells"]))
            cgroups.setdefault(sig, []).append(k)
        for sig, members in cgroups.items():
            done = next((m for m in members
                         if str(st["clusters"][m]["time"]) in st["clusters"][m]["results"]), None)
            if done:
                res = st["clusters"][done]["results"][str(st["clusters"][done]["time"])]
            else:
                done = members[0]
                res = run_cluster(st, done)
            copies = [m for m in members if m != done
                      and str(st["clusters"][m]["time"]) not in st["clusters"][m]["results"]]
            for m in copies:
                st["clusters"][m]["results"][str(st["clusters"][m]["time"])] = {**res, "via": done}
            if copies:
                save_state(st)
                append_results(
                    f"| {', '.join(copies)} | fault-cluster dil-1 (same model as {done}) "
                    f"| {st['clusters'][done]['n_cells']} | {st['clusters'][done]['time']} "
                    f"| {res['status']} | via {done} | {res.get('win_edges')} "
                    f"| {res.get('inc_gain')} | {now()} |")

    print(f"[{now()}] ladder pass complete, no SAT. See docs/phase0_results.md")


if __name__ == "__main__":
    main()

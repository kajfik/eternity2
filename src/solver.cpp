// Eternity II max-edges solver
//
// Pipeline:
//   1. Hunter threads: randomized perfect border ring, then row-major interior
//      DFS with a mismatch budget that self-tightens as the global best
//      improves. Color-availability deficit bound prunes hopeless subtrees.
//   2. Polisher threads: large-neighborhood search on complete boards -
//      remove a small window (rectangles or fault-grown blobs), re-solve it
//      exactly by branch & bound to maximize matched edges.
//
// Board encoding matches e2.bucas.name: 16x16 row-major, per-cell edge quad
// (Up,Right,Down,Left), motif 0 = grey border. Score = matched internal
// edges, max 480. Clue cells are fixed pieces and never moved.
//
// Build: g++ -O3 -march=native -std=c++20 -static -pthread solver.cpp

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "e2data.h"

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
namespace fs = std::filesystem;

// ------------------------------------------------------------------ config
struct Config {
    int clues = 5;              // 1 or 5
    int threads = 0;            // 0 = auto
    double polish_frac = 0.5;   // fraction of effort spent polishing (mix mode)
    std::string mode = "mix";   // hunt | polish | mix
    std::string out_dir = "runs";
    u64 node_cap = 40'000'000;  // per hunter restart
    int budget_cap = 48;        // max mismatches accepted in a completion
    int pre_budget = 3;         // mismatches allowed before free_row
    int seed_slack = 8;         // keep seeds with m <= best_m + slack
    int fr_min = 6, fr_max = 12;// per-restart random perfect-prefix row
    int region_max = 16;        // max cells removed per LNS B&B window
    int scatter_max = 40;       // max cells in Hungarian reassignment move
    u64 region_node_cap = 2'000'000;
    int stagnation_kick = 4000; // LNS iters without improvement -> perturb
    int band_stagnation = 3000; // LNS iters without improvement -> band rebuild
    u64 band_node_cap = 30'000'000;  // per band rebuild attempt
    double minutes = 0;         // stop after this many minutes (0 = run forever)
    std::string seed_url;       // optional: start polishing from this board
    bool selftest = false;      // rotation identity/invariance checks, then exit
    // --- umbrella distance-kick + LAHC acceptance (task #4) ---
    bool umbrella = true;              // --umbrella 0/1
    int umbrella_dist = 32;            // base blob size, jittered 25-40
    int umbrella_stagnation = 8000;    // LNS iters without gain -> forced jump
    int explore_pct = 10;              // % of region moves run without incumbent seeding
    int lahc_len = 50;                 // LAHC history length; 0 disables LAHC+explore
    // --- experimental (SOLVER_EVAL tracks) ---
    // candidate ordering: default|drarity|random|model|zone|model+zone.
    // drarity = directional rarity (SOLVER_EVAL Track A finding): spend
    // scarce colors on the matched W/N sides, avoid demanding them on the
    // open E/S sides — the no-ML form of what the learned model does.
    std::string order = "default";
    std::string model_file;         // weights for order=model (Track A)
    std::string zone_file;          // per-color band weights for order=zone (Track C)
    std::string log_placements;     // log deepest-path placements per restart (Track A data)
    double rec_seconds = 15;        // decomp: seam-polish time in reconciliation (Track B)
    // --- plateau-merge drift snapshots (task #5 Part A) ---
    std::string drift_dir;          // empty = off (default); else dir for drift_t<id>.txt
    // --- double-break + parity moves (raphael-anjou research adoptions) ---
    // cost-2 candidate tier (both W and N mismatch). Record boards contain
    // double-break cells (JB470 at (2,10), R461 at (14,14)); without this
    // tier the DFS move space provably excludes their board family.
    // 0 = off, 1 = BandSolver only (default), 2 = also Hunter DFS.
    int cost2 = 1;
    int parity_pct = 2;             // % of polish steps: exact LAP reassignment of a
                                    // whole chessboard-parity class (~97 cells); 0 = off
    // deepest band start row for rebuilds (y0 range = [band_y0_min, 12]).
    // Default 8 = old behavior (42-98 free cells). Lower toward 1 to allow
    // sigma-cycle-scale backtracks (80-154+ cells) from a record board --
    // completions become needle-rare, so pair with a much larger --band-nodes.
    int band_y0_min = 8;
    // --- from-scratch pipeline upgrades (2026-07-08 analysis) ---
    // hybrid completion: every hunter restart replays its deepest DFS prefix
    // and greedy-fills the rest, so restarts always emit a fresh-lineage
    // board (~perfect top rows + greedy tail) instead of nothing.
    bool hybrid = true;              // --hybrid 0/1
    int tie_pct = 100;               // % of equal-score LNS results applied
                                     // (sideways drift speed; 30 = old behavior)
    int merge_pct = 2;               // % of polish steps: exact plateau merge
    u64 merge_node_cap = 16'000'000; // B&B nodes per merge window
    int plateau_cap = 4096;          // near-best drift archive size (deduped)
    int region_big_pct = 10;         // % of region moves using the larger cap
    int region_max_big = 24;         // cells in the larger exact windows (<=40)
    // --- Verhaard adoptions (shortestpath.se/eii/eii_details.html, 2026-07-08) ---
    // good-groups piece scheduling: per restart one group is sampled; its
    // members are "good" (high-frequency members "precious"), non-members
    // "bad" (low-frequency non-members "useless"). Checkpoints constrain WHICH
    // pieces may be used at which interior depth: bad/useless early, good
    // late (LV's factor-100 heuristic).
    std::string groups_file;         // from --mode make-groups; empty = off
    int group_order = 1;             // 1 = class-priority bucket ordering
                                     // (useless>bad>good>precious): spend the
                                     // worst-tiling pieces early WITHOUT
                                     // pruning legal moves. LV's hard gates
                                     // (below) collapse our interior-only
                                     // perfect-prefix DFS to avgdepth<10, so
                                     // ordering is the default adaptation.
    int group_maxgood = 6;           // good pieces allowed before group_ck[1]
    int group_n_prec = 16;           // precious = top-N by cross-group frequency
    int group_n_useless = 16;        // useless = bottom-N
    int group_ck[4] = {0, 0, 0, 0};  // HARD interior-depth gates, 0 = off:
                                     // [0] no good before, [1] <=maxgood before,
                                     // [2] all useless used by, [3] no precious
                                     // before. LV's 63/79/96/100 rescale to
                                     // 42,56,70,74 of 191 interior cells --
                                     // measured lethal here (avgdepth 7-63 vs
                                     // 98 baseline); kept for experiments.
    // graded slip schedule (LV "edge slipping"): cumulative per-depth mismatch
    // caps sized for score >= slip_target -- perfect prefix, caps rising with
    // decreasing gaps over the last slip_span of the scan. Replaces the
    // free_row/pre_budget step function when set.
    int slip_target = 0;             // 0 = off
    double slip_span = 0.30;
    std::string abort_points;        // adaptive restart aborts "nodes:mindepth,..."
    int tail_cols = 0;               // last N interior rows scanned column-wise
    // --mode make-groups parameters
    int group_size = 98;             // good-group size (of 196 interior pieces)
    int groups_n = 60;               // groups to generate
    int groups_iters = 1200;         // hill-climb swaps per group
    std::string groups_out = "eval/groups.txt";
};
static Config cfg;

// ------------------------------------------------------------------ static data
static constexpr int W = 16, H = 16, NC = 256;
static constexpr int MAX_SCORE = 480;

// quad of piece p at rotation k (cw): q[i] = def[(i - k) & 3]
static u8 QUAD[256][4][4];              // [piece][rot][dir] dir:0=U 1=R 2=D 3=L
static int PTYPE[256];                  // 0 interior, 1 border(edge), 2 corner
static int CELL_TYPE[NC];               // 0 interior, 1 side, 2 corner
static bool IS_CLUE_CELL[NC];
static int CLUE_AT[NC];                  // piece index or -1
static int CLUE_ROT[NC];

// static scarcity weight per color: how many sides of that color exist among
// interior pieces (the only pieces the hunter DFS ever places). Colors 1-5
// never appear on interior pieces at all (avail is always 0 for them, and the
// existing per-color deficit bound already handles that exactly); among the
// remaining colors the count still varies ~10%, so candidates are ordered to
// place scarcer-color sides earlier while more neighbor slots are still open,
// rather than leaving them to random luck near the end of a restart.
static int COLOR_RARITY[23];            // higher = rarer among interior pieces

// ------------------------------------------------------------------ ordering seam (Tracks A/C)
// The hunter's per-node candidate ordering is realized as a per-bucket sort at
// restart setup: bucket exact[w][n] holds exactly the candidates whose west
// side = w and north side = n, so for the row-major scan a bucket ordering IS
// a state-conditioned ordering on (candidate quad, matched W/N neighbors);
// the E/S neighbors are always still empty (encoded 0). Scorers plugged in
// here therefore see the same 8 inputs as the offline model:
// (cU,cR,cD,cL, nN,nE=0,nS=0,nW) with nN=cU, nW=cL in the exact buckets.
// Components are z-normalized over all candidates at build time and summed
// according to cfg.order, so arms are comparable without manual scaling.
static int ZONE_BAND[NC];               // ring distance - 1 (0..6) for interior cells
static constexpr int N_BANDS = 7;
static float ZONE_W[23][N_BANDS];       // per (color, band) log-affinity, from --zone-file
static bool g_zone_loaded = false;

// model: LINEAR (8 slots x 23 colors one-hot -> score) or MLP with one
// hidden layer over the same 184-dim one-hot input
struct ModelWeights {
    bool loaded = false;
    bool mlp = false;
    int hidden = 0;
    std::vector<float> W1, b1, w2;  // mlp: W1[h][184], b1[h], w2[h]
    float b2 = 0, lin[8 * 23] = {}; // linear: slot*23+color
    float score(const u8 cand[4], const u8 nbr[4]) const {
        // slots 0-3: candidate U,R,D,L; slots 4-7: neighbor N,E,S,W colors
        if (!mlp) {
            float s = b2;
            for (int i = 0; i < 4; i++) s += lin[i * 23 + cand[i]];
            for (int i = 0; i < 4; i++) s += lin[(4 + i) * 23 + nbr[i]];
            return s;
        }
        float s = b2;
        for (int h = 0; h < hidden; h++) {
            float a = b1[h];
            const float* w = &W1[h * 184];
            for (int i = 0; i < 4; i++) a += w[i * 23 + cand[i]];
            for (int i = 0; i < 4; i++) a += w[(4 + i) * 23 + nbr[i]];
            s += w2[h] * (a > 0 ? a : 0);
        }
        return s;
    }
};
static ModelWeights g_model;

static bool load_model(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string kind;
    f >> kind;
    if (kind == "LINEAR") {
        for (int i = 0; i < 8 * 23; i++) f >> g_model.lin[i];
        f >> g_model.b2;
        g_model.mlp = false;
    } else if (kind == "MLP") {
        f >> g_model.hidden;
        g_model.mlp = true;
        g_model.W1.resize(g_model.hidden * 184);
        g_model.b1.resize(g_model.hidden);
        g_model.w2.resize(g_model.hidden);
        for (auto& v : g_model.W1) f >> v;
        for (auto& v : g_model.b1) f >> v;
        for (auto& v : g_model.w2) f >> v;
        f >> g_model.b2;
    } else return false;
    g_model.loaded = (bool)f;
    return g_model.loaded;
}

static bool load_zone(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    for (int c = 0; c < 23; c++)
        for (int b = 0; b < N_BANDS; b++) f >> ZONE_W[c][b];
    g_zone_loaded = (bool)f;
    return g_zone_loaded;
}

// ------------------------------------------------------------------ good groups (LV, shortestpath.se)
// File lines: <tileability> <piece ids, 1-based>, written by --mode
// make-groups. Precious/useless are global classes from cross-group
// frequency: pieces in nearly every optimized group tile best, pieces in
// almost none tile worst.
static std::vector<std::vector<int>> g_groups;
static bool GROUP_PREC[256], GROUP_USELESS[256];
static bool g_groups_loaded = false;

static bool load_groups(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    int freq[256] = {};
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        long long til;
        if (!(is >> til)) return false;
        std::vector<int> g;
        int pid;
        while (is >> pid) {
            if (pid < 1 || pid > 256 || PTYPE[pid - 1] != 0) return false;
            g.push_back(pid - 1);
            freq[pid - 1]++;
        }
        if (g.empty()) return false;
        g_groups.push_back(std::move(g));
    }
    if (g_groups.empty()) return false;
    std::vector<int> ids;
    for (int p = 0; p < 256; p++)
        if (PTYPE[p] == 0) ids.push_back(p);
    std::sort(ids.begin(), ids.end(),
              [&](int a, int b) { return freq[a] > freq[b]; });
    for (int i = 0; i < cfg.group_n_prec && i < (int)ids.size(); i++)
        GROUP_PREC[ids[i]] = true;
    for (int i = 0; i < cfg.group_n_useless && i < (int)ids.size(); i++)
        GROUP_USELESS[ids[(int)ids.size() - 1 - i]] = true;
    g_groups_loaded = true;
    return true;
}

// adaptive restart aborts (LV's factor-5): sorted (nodes, min max_depth)
// milestones; a restart whose deepest depth lags the milestone is killed
static std::vector<std::pair<u64, int>> g_abort_ms;

static void init_static(int nclues) {
    for (int p = 0; p < 256; p++) {
        int g = 0;
        for (int i = 0; i < 4; i++) g += e2::PIECE_DEFS[p][i] == 0;
        PTYPE[p] = g == 0 ? 0 : (g == 1 ? 1 : 2);
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < 4; i++)
                QUAD[p][k][i] = e2::PIECE_DEFS[p][(i - k) & 3];
    }
    int interior_count[23] = {};
    for (int p = 0; p < 256; p++)
        if (PTYPE[p] == 0)
            for (int i = 0; i < 4; i++) interior_count[e2::PIECE_DEFS[p][i]]++;
    int max_count = 0;
    for (int c = 1; c <= 22; c++) max_count = std::max(max_count, interior_count[c]);
    for (int c = 1; c <= 22; c++)
        COLOR_RARITY[c] = interior_count[c] > 0 ? max_count - interior_count[c] : 0;
    for (int s = 0; s < NC; s++) {
        int x = s % W, y = s / W;
        bool bx = x == 0 || x == W - 1, by = y == 0 || y == H - 1;
        CELL_TYPE[s] = bx && by ? 2 : (bx || by ? 1 : 0);
        int rd = std::min(std::min(x, W - 1 - x), std::min(y, H - 1 - y));
        ZONE_BAND[s] = std::max(0, std::min(N_BANDS - 1, rd - 1));
        IS_CLUE_CELL[s] = false;
        CLUE_AT[s] = -1;
        CLUE_ROT[s] = 0;
    }
    for (int i = 0; i < e2::N_CLUES; i++) {
        int s = e2::CLUES[i][0], p = e2::CLUES[i][1] - 1, r = e2::CLUES[i][2];
        if (nclues == 1 && p != 138) continue;  // piece 139 only
        IS_CLUE_CELL[s] = true;
        CLUE_AT[s] = p;
        CLUE_ROT[s] = r;
    }
}

// ------------------------------------------------------------------ rng
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
    u64 next() {
        s += 0x9e3779b97f4a7c15ULL;
        u64 z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    u32 below(u32 n) { return (u32)(((next() >> 32) * (u64)n) >> 32); }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

// ------------------------------------------------------------------ board
struct Board {
    u8 piece[NC];   // 0..255, valid when filled
    u8 rot[NC];
    bool filled[NC];

    const u8* q(int s) const { return QUAD[piece[s]][rot[s]]; }

    int score() const {  // matched internal edges (grey-grey does not count)
        int m = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int s = y * W + x;
                if (!filled[s]) continue;
                if (x + 1 < W && filled[s + 1]) {
                    u8 a = q(s)[1], b = q(s + 1)[3];
                    if (a == b && a != 0) m++;
                }
                if (y + 1 < H && filled[s + W]) {
                    u8 a = q(s)[2], b = q(s + W)[0];
                    if (a == b && a != 0) m++;
                }
            }
        return m;
    }
    std::string edges_string() const {
        std::string e(NC * 4, 'a');
        for (int s = 0; s < NC; s++)
            if (filled[s])
                for (int i = 0; i < 4; i++) e[s * 4 + i] = (char)('a' + q(s)[i]);
        return e;
    }
    std::string url() const {
        std::string u = "https://e2.bucas.name/#puzzle=E2Solver&board_w=16&board_h=16&board_edges=";
        u += edges_string();
        u += "&board_pieces=";
        char buf[8];
        for (int s = 0; s < NC; s++) {
            snprintf(buf, sizeof buf, "%03d", filled[s] ? piece[s] + 1 : 0);
            u += buf;
        }
        u += "&show_conflicts=1";
        return u;
    }
};

// map cell s through a k*90cw whole-board rotation (k=0..3), W==H square
static inline int rotate_cell(int s, int k) {
    int x = s % W, y = s / W;
    for (int i = 0; i < k; i++) { int nx = W - 1 - y, ny = x; x = nx; y = ny; }
    return y * W + x;
}

// whole-board rotation by orient*90cw: cell (x,y) -> rotate_cell mapping,
// piece rotation rot' = (rot+orient)&3. Preserves all edge matches and rim
// grey-correctness (W==H).
static Board rotate_board(const Board& b, int orient) {
    Board out;
    memset(&out, 0, sizeof out);
    orient &= 3;
    for (int s = 0; s < NC; s++) {
        if (!b.filled[s]) continue;
        int ds = rotate_cell(s, orient);
        out.piece[ds] = b.piece[s];
        out.rot[ds] = (u8)((b.rot[s] + orient) & 3);
        out.filled[ds] = true;
    }
    return out;
}

// ------------------------------------------------------------------ global best + seed pool
// FNV-1a over (piece,rot,filled) per cell: identifies a board arrangement.
// Used to deduplicate the pools -- without it, every incremental gain of one
// lineage re-inserts near-copies and random eviction erases real diversity.
static u64 board_hash(const Board& b) {
    u64 h = 14695981039346656037ULL;
    for (int s = 0; s < NC; s++) {
        u64 x = b.filled[s] ? (((u64)b.piece[s] << 2) | b.rot[s]) + 1 : 0;
        h = (h ^ x) * 1099511628211ULL;
    }
    return h;
}

struct SeedPool {
    std::mutex mu;
    std::vector<Board> pool;
    std::vector<u64> hs;            // hash per pool slot
    std::unordered_set<u64> have;   // dedup: arrangements currently in pool
    size_t cap = 160;
    u64 tick = 0x2545F4914F6CDD1DULL;
    bool add(const Board& b) {
        u64 h = board_hash(b);
        std::lock_guard<std::mutex> lk(mu);
        if (!have.insert(h).second) return false;  // exact duplicate
        tick = tick * 6364136223846793005ULL + 1442695040888963407ULL;
        if (pool.size() < cap) {
            pool.push_back(b);
            hs.push_back(h);
        } else {
            size_t i = (tick >> 33) % cap;
            have.erase(hs[i]);
            pool[i] = b;
            hs[i] = h;
        }
        return true;
    }
    bool get(Rng& rng, Board& out) {
        std::lock_guard<std::mutex> lk(mu);
        if (pool.empty()) return false;
        out = pool[rng.below((u32)pool.size())];
        return true;
    }
    size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return pool.size();
    }
};

static std::atomic<int> g_best_score{-1};
static std::mutex g_best_mu;
static Board g_best_board;
static SeedPool g_seeds;   // elite: within seed_slack of global best
static SeedPool g_fresh;   // random sample of all completions (lineage diversity)
static SeedPool g_plateau; // near-best drift archive (deduped, cap plateau_cap):
                           // the raw material for merge_move -- the 464 author's
                           // mass same-score harvest, in-process
static std::mutex g_merge_mu;
static std::unordered_set<u64> g_merge_tried;  // symmetric pair hashes already merged
static std::atomic<u64> g_nodes{0}, g_restarts{0}, g_completions{0},
    g_polish_iters{0}, g_polish_gains{0}, g_depth_sum{0},
    g_scatter_iters{0}, g_scatter_gains{0}, g_umbrella_jumps{0},
    g_region_tieproof{0}, g_parity_iters{0}, g_parity_gains{0},
    g_hybrid{0}, g_merge_iters{0}, g_merge_gains{0}, g_aborts{0};
static std::atomic<int> g_depth_max{0};
static std::atomic<bool> g_stop{false};
static std::mutex g_log_mu;
static std::ofstream g_log;   // placement log (Track A), opened in main

static std::string best_path() {
    return cfg.out_dir + "/best_c" + std::to_string(cfg.clues) + ".txt";
}

static void save_board(const Board& b, int score, const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    f << "# eternity2 clues=" << cfg.clues << " score=" << score << "/480\n";
    for (int s = 0; s < NC; s++)
        f << (int)(b.piece[s] + 1) << ":" << (int)b.rot[s] << (s % 16 == 15 ? "\n" : " ");
    f << b.edges_string() << "\n";
    f << b.url() << "\n";
}

// returns true if this is a new global best
static bool report_completion(const Board& b) {
    int sc = b.score();
    int cur = g_best_score.load(std::memory_order_relaxed);
    if (sc + cfg.seed_slack >= cur) g_seeds.add(b);
    g_fresh.add(b);
    if (sc + 1 >= cur) g_plateau.add(b);
    while (sc > cur) {
        if (g_best_score.compare_exchange_weak(cur, sc)) {
            std::lock_guard<std::mutex> lk(g_best_mu);
            g_best_board = b;
            save_board(b, sc, best_path());
            char hist[128];
            time_t t = time(nullptr);
            struct tm tmv = *localtime(&t);  // called under g_best_mu only
            snprintf(hist, sizeof hist, "%04d-%02d-%02d %02d:%02d:%02d score=%d",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec, sc);
            std::ofstream hf(cfg.out_dir + "/history_c" + std::to_string(cfg.clues) + ".log",
                             std::ios::app);
            hf << hist << " " << b.url() << "\n";
            printf("\n[best] %s  -> %s\n", hist, best_path().c_str());
            fflush(stdout);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ ring solver
// Perimeter cells in clockwise order starting at (0,0).
static int RING_CELLS[60];
static int ring_rot_for_cell(int s, int p) {
    // rotation making grey sides point outward; -1 if impossible
    int x = s % W, y = s / W;
    for (int k = 0; k < 4; k++) {
        const u8* q = QUAD[p][k];
        bool ok = (q[0] == 0) == (y == 0) && (q[2] == 0) == (y == H - 1) &&
                  (q[3] == 0) == (x == 0) && (q[1] == 0) == (x == W - 1);
        if (ok) return k;
    }
    return -1;
}

static void init_ring_cells() {
    int i = 0;
    for (int x = 0; x < W; x++) RING_CELLS[i++] = x;                    // top
    for (int y = 1; y < H; y++) RING_CELLS[i++] = y * W + (W - 1);      // right
    for (int x = W - 2; x >= 0; x--) RING_CELLS[i++] = (H - 1) * W + x; // bottom
    for (int y = H - 2; y >= 1; y--) RING_CELLS[i++] = y * W;           // left
}

// exposed color of ring cell a toward ring cell b (consecutive on ring)
static int side_toward(const Board& b, int sa, int sb) {
    int dx = sb % W - sa % W, dy = sb / W - sa / W;
    const u8* q = b.q(sa);
    if (dx == 1) return q[1];
    if (dx == -1) return q[3];
    if (dy == 1) return q[2];
    return q[0];
}

static bool solve_ring(Board& b, Rng& rng) {
    // randomized DFS over the 60 perimeter cells; corners and edges apart
    std::vector<int> corners, edges;
    for (int p = 0; p < 256; p++)
        if (PTYPE[p] == 2) corners.push_back(p);
        else if (PTYPE[p] == 1) edges.push_back(p);
    std::shuffle(corners.begin(), corners.end(), std::mt19937_64(rng.next()));
    std::shuffle(edges.begin(), edges.end(), std::mt19937_64(rng.next()));

    bool used[256] = {};
    int cand_idx[60];
    memset(cand_idx, 0, sizeof cand_idx);
    int d = 0;
    u64 guard = 0;
    while (d < 60) {
        if (++guard > 3'000'000) return false;  // pathological shuffle; retry
        int s = RING_CELLS[d];
        auto& list = CELL_TYPE[s] == 2 ? corners : edges;
        bool placed = false;
        for (int& i = cand_idx[d]; i < (int)list.size(); i++) {
            int p = list[i];
            if (used[p]) continue;
            int k = ring_rot_for_cell(s, p);
            if (k < 0) continue;
            b.piece[s] = (u8)p;
            b.rot[s] = (u8)k;
            b.filled[s] = true;
            // must match previous ring neighbor
            if (d > 0) {
                int sp = RING_CELLS[d - 1];
                if (side_toward(b, sp, s) != side_toward(b, s, sp)) { b.filled[s] = false; continue; }
            }
            if (d == 59) {  // closure with RING_CELLS[0]
                int s0 = RING_CELLS[0];
                if (side_toward(b, s, s0) != side_toward(b, s0, s)) { b.filled[s] = false; continue; }
            }
            used[p] = true;
            placed = true;
            i++;
            break;
        }
        if (placed) { d++; if (d < 60) cand_idx[d] = 0; }
        else {
            b.filled[RING_CELLS[d]] = false;
            if (d == 0) return false;
            d--;
            int sp = RING_CELLS[d];
            used[b.piece[sp]] = false;
            b.filled[sp] = false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ hunter
// candidate entry: (p<<22)|(rot<<20)|(n<<15)|(e<<10)|(s<<5)|w
static inline u32 pack(int p, int r, const u8* q) {
    return ((u32)p << 22) | ((u32)r << 20) | ((u32)q[0] << 15) | ((u32)q[1] << 10) |
           ((u32)q[2] << 5) | (u32)q[3];
}
#define CP(v) ((v) >> 22)
#define CR(v) (((v) >> 20) & 3)
#define CN(v) (((v) >> 15) & 31)
#define CE(v) (((v) >> 10) & 31)
#define CS(v) (((v) >> 5) & 31)
#define CW(v) ((v) & 31)

struct Buckets {
    // [band][w][n]; band dimension collapses to 1 unless zone ordering is on
    std::vector<u32> exact[N_BANDS][23][23];
    std::vector<u32> by_w[N_BANDS][23];   // north differs
    std::vector<u32> by_n[N_BANDS][23];   // west differs
    std::vector<u32> all_list[N_BANDS];   // every candidate, same value order
                                          // (cost-2 tier iterates this, skipping
                                          // W-match and N-match entries)
    int n_bands = 1;

    void build(Rng& rng, const bool* piece_excluded, const u8* pcls = nullptr) {
        bool use_zone = g_zone_loaded &&
            (cfg.order == "zone" || cfg.order == "model+zone");
        bool use_model = g_model.loaded &&
            (cfg.order == "model" || cfg.order == "model+zone");
        bool use_rarity = cfg.order == "default" || cfg.order == "zone";
        bool use_drarity = cfg.order == "drarity";
        n_bands = use_zone ? N_BANDS : 1;
        for (int b = 0; b < N_BANDS; b++) {
            for (auto& row : exact[b]) for (auto& v : row) v.clear();
            for (auto& v : by_w[b]) v.clear();
            for (auto& v : by_n[b]) v.clear();
            all_list[b].clear();
        }
        std::vector<u32> all;
        for (int p = 0; p < 256; p++) {
            if (PTYPE[p] != 0 || piece_excluded[p]) continue;
            for (int k = 0; k < 4; k++) all.push_back(pack(p, k, QUAD[p][k]));
        }
        std::shuffle(all.begin(), all.end(), std::mt19937_64(rng.next()));
        size_t n = all.size();
        // component scores (rarity / learned model / zone affinity), each
        // z-normalized across the candidate set so cfg.order arms combine on
        // equal footing. Ties keep shuffle order (stable_sort), so "random"
        // = shuffle only, exactly the pre-existing behavior minus rarity.
        auto znorm = [&](std::vector<float>& v) {
            double mu = 0, s2 = 0;
            for (float x : v) mu += x;
            mu /= (double)v.size();
            for (float x : v) s2 += (x - mu) * (x - mu);
            double sd = std::sqrt(s2 / (double)v.size());
            if (sd < 1e-9) sd = 1;
            for (float& x : v) x = (float)((x - mu) / sd);
        };
        std::vector<float> base(n, 0.f);
        if (use_rarity || use_drarity) {
            // matched sides (W,N) are consumed by this placement; open sides
            // (E,S) become demands on future neighbors. Symmetric rarity
            // rewards scarce colors everywhere; directional rarity rewards
            // consuming them and penalizes demanding them.
            float open_sign = use_drarity ? -1.f : 1.f;
            std::vector<float> r(n);
            for (size_t i = 0; i < n; i++) {
                u32 v = all[i];
                r[i] = (float)(COLOR_RARITY[CN(v)] + COLOR_RARITY[CW(v)]) +
                       open_sign * (float)(COLOR_RARITY[CE(v)] + COLOR_RARITY[CS(v)]);
            }
            znorm(r);
            for (size_t i = 0; i < n; i++) base[i] += r[i];
        }
        if (use_model) {
            std::vector<float> m(n);
            for (size_t i = 0; i < n; i++) {
                u32 v = all[i];
                u8 cand[4] = {(u8)CN(v), (u8)CE(v), (u8)CS(v), (u8)CW(v)};
                u8 nbr[4] = {(u8)CN(v), 0, 0, (u8)CW(v)};  // matched W/N context
                m[i] = g_model.score(cand, nbr);
            }
            znorm(m);
            for (size_t i = 0; i < n; i++) base[i] += m[i];
        }
        if (pcls) {
            // class-priority ordering (LV good groups as value ordering):
            // useless > bad > good > precious, dominant over other components
            // so the DFS spends the worst-tiling pieces first and holds the
            // best-tiling ones for the tail; other components order within a
            // class. No legal move is ever pruned.
            static const float PRIO[4] = {2.f, 1.f, 0.f, 3.f};  // bad,good,prec,useless
            for (size_t i = 0; i < n; i++)
                base[i] += 1000.f * PRIO[pcls[CP(all[i])]];
        }
        std::vector<size_t> idx(n);
        std::vector<float> sc(n);
        for (int b = 0; b < n_bands; b++) {
            for (size_t i = 0; i < n; i++) {
                sc[i] = base[i];
                idx[i] = i;
            }
            if (use_zone) {
                std::vector<float> z(n);
                for (size_t i = 0; i < n; i++) {
                    u32 v = all[i];
                    z[i] = ZONE_W[CN(v)][b] + ZONE_W[CE(v)][b] +
                           ZONE_W[CS(v)][b] + ZONE_W[CW(v)][b];
                }
                znorm(z);
                for (size_t i = 0; i < n; i++) sc[i] += z[i];
            }
            std::stable_sort(idx.begin(), idx.end(),
                             [&](size_t a, size_t c) { return sc[a] > sc[c]; });
            for (size_t i : idx) {
                u32 v = all[i];
                exact[b][CW(v)][CN(v)].push_back(v);
                by_w[b][CW(v)].push_back(v);
                by_n[b][CN(v)].push_back(v);
                all_list[b].push_back(v);
            }
        }
    }
};

struct Hunter {
    Rng rng;
    Buckets bk;
    Board b;
    bool used[256];
    int D[23];       // demanded half-edges (filled sides facing unfilled cells)
    int avail[23];   // sides on unused interior pieces
    int deficit;     // sum_c max(0, D[c]-avail[c])
    int scan[196];   // interior non-clue cells in row-major order
    int n_scan;
    u64 nodes, node_cap;
    int budget;      // total mismatches allowed (dominated by tail rows)
    int free_row;    // interior rows >= this may take mismatches freely
    int max_depth;   // deepest scan index reached this restart

    // placement logging (Track A training data): current DFS path and the
    // deepest snapshot of it this restart; outcome label = snapshot depth
    bool log_on = false;
    u16 lp_cell[196]; u8 lp_p[196], lp_r[196], lp_w[196], lp_n[196];
    u16 sp_cell[196]; u8 sp_p[196], sp_r[196], sp_w[196], sp_n[196];
    int snap_depth = 0;
    // hybrid completion: piece/rot of the deepest DFS prefix this restart
    // (captured whenever max_depth improves; scan order indices)
    u8 hyb_p[196], hyb_r[196];
    // --- Verhaard adoptions (all flag-gated, off by default) ---
    bool grp_on = false;     // good-groups piece-class schedule this restart
    u8 pclass[256];          // 0 bad, 1 good, 2 precious, 3 useless
    int good_used = 0, useless_left = 0;
    bool slip_on = false;    // graded cumulative mismatch schedule
    int slip_cap_arr[200];   // max total cost after placing scan[d]
    int ms_idx = 0;          // next adaptive-abort milestone in g_abort_ms

    explicit Hunter(u64 seed) : rng(seed) { log_on = !cfg.log_placements.empty(); }

    void bump_D(int c, int dv) {
        int before = std::max(0, D[c] - avail[c]);
        D[c] += dv;
        deficit += std::max(0, D[c] - avail[c]) - before;
    }
    void bump_avail(int c, int dv) {
        int before = std::max(0, D[c] - avail[c]);
        avail[c] += dv;
        deficit += std::max(0, D[c] - avail[c]) - before;
    }

    // place piece p rot k at cell s; returns mismatch cost vs filled neighbors
    int place(int s, int p, int k) {
        const u8* q = QUAD[p][k];
        b.piece[s] = (u8)p;
        b.rot[s] = (u8)k;
        b.filled[s] = true;
        used[p] = true;
        int cost = 0;
        static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
        for (int dir = 0; dir < 4; dir++) {
            int nx = s % W + DX[dir], ny = s / W + DY[dir];
            int ns = ny * W + nx;
            int opp = (dir + 2) & 3;
            if (b.filled[ns]) {
                u8 other = b.q(ns)[opp];
                if (other != q[dir]) cost++;
                bump_D(other, -1);
            } else {
                bump_D(q[dir], +1);
            }
        }
        for (int i = 0; i < 4; i++) bump_avail(q[i], -1);
        return cost;
    }
    void unplace(int s) {
        int p = b.piece[s];
        const u8* q = b.q(s);
        for (int i = 0; i < 4; i++) bump_avail(q[i], +1);
        static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
        b.filled[s] = false;
        for (int dir = 0; dir < 4; dir++) {
            int ns = (s / W + DY[dir]) * W + s % W + DX[dir];
            int opp = (dir + 2) & 3;
            if (b.filled[ns]) bump_D(b.q(ns)[opp], +1);
            else bump_D(q[dir], -1);
        }
        used[p] = false;
    }

    bool setup_restart() {
        memset(&b, 0, sizeof b);
        memset(used, 0, sizeof used);
        if (!solve_ring(b, rng)) return false;
        for (int s = 0; s < NC; s++)
            if (b.filled[s]) used[b.piece[s]] = true;
        // clues
        for (int s = 0; s < NC; s++)
            if (IS_CLUE_CELL[s]) {
                b.piece[s] = (u8)CLUE_AT[s];
                b.rot[s] = (u8)CLUE_ROT[s];
                b.filled[s] = true;
                used[CLUE_AT[s]] = true;
            }
        bool excl[256] = {};
        for (int s = 0; s < NC; s++)
            if (IS_CLUE_CELL[s]) excl[CLUE_AT[s]] = true;
        // good-groups piece classes for this restart (sampled before the
        // bucket build so class-priority ordering can use them)
        grp_on = g_groups_loaded;
        if (grp_on) {
            const auto& gg = g_groups[rng.below((u32)g_groups.size())];
            memset(pclass, 0, sizeof pclass);
            for (int p : gg) pclass[p] = GROUP_PREC[p] ? 2 : 1;
            for (int p = 0; p < 256; p++)
                if (pclass[p] == 0 && GROUP_USELESS[p]) pclass[p] = 3;
            good_used = 0;
            useless_left = 0;
            for (int p = 0; p < 256; p++)
                if (pclass[p] == 3 && !used[p]) useless_left++;
        }
        bk.build(rng, excl, grp_on && cfg.group_order ? pclass : nullptr);
        // demands/avail
        memset(D, 0, sizeof D);
        memset(avail, 0, sizeof avail);
        deficit = 0;
        for (int p = 0; p < 256; p++)
            if (!used[p] && PTYPE[p] == 0)
                for (int i = 0; i < 4; i++) avail[e2::PIECE_DEFS[p][i]]++;
        for (int s = 0; s < NC; s++) {
            if (!b.filled[s]) continue;
            static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
            for (int dir = 0; dir < 4; dir++) {
                int nx = s % W + DX[dir], ny = s / W + DY[dir];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                if (!b.filled[ny * W + nx]) bump_D(b.q(s)[dir], +1);
            }
        }
        n_scan = 0;
        // column-wise tail (LV search-order finding): the last tail_cols
        // interior rows are scanned column-by-column, so late cells see two
        // decided neighbors while the slip allowance is active. Every scan
        // cell still has decided W and N neighbors, so the bucket scheme is
        // unchanged.
        int tail_y0 = cfg.tail_cols > 0 ? std::max(1, H - 1 - cfg.tail_cols)
                                        : H - 1;
        for (int y = 1; y < tail_y0; y++)
            for (int x = 1; x < W - 1; x++)
                if (!IS_CLUE_CELL[y * W + x]) scan[n_scan++] = y * W + x;
        for (int x = 1; x < W - 1; x++)
            for (int y = tail_y0; y < H - 1; y++)
                if (!IS_CLUE_CELL[y * W + x]) scan[n_scan++] = y * W + x;
        free_row = cfg.fr_min + (int)rng.below((u32)(cfg.fr_max - cfg.fr_min + 1));
        if (cfg.tail_cols > 0) free_row = std::min(free_row, tail_y0);
        int cur_best = g_best_score.load(std::memory_order_relaxed);
        int best_m = cur_best < 0 ? 1 << 20 : MAX_SCORE - cur_best;
        budget = std::min(cfg.budget_cap, best_m + cfg.seed_slack);
        // graded slip schedule: cap k (k=1..T) unlocks at a sqrt-spaced depth
        // in the slip zone -- gaps shrink toward the end (LV's shape), and the
        // last ~span/10 cells get no new slips, mirroring his final-16 hold.
        slip_on = cfg.slip_target > 0;
        if (slip_on) {
            int T = MAX_SCORE - cfg.slip_target;
            budget = std::min(budget, T);
            int span = std::min(n_scan, std::max(T, (int)(cfg.slip_span * n_scan)));
            int d0 = n_scan - span;
            int rise = std::max(1, span - std::max(4, span / 10));
            for (int d = 0; d < n_scan; d++) slip_cap_arr[d] = 0;
            for (int k = 1; k <= T; k++) {
                int dk = d0 + (int)std::lround(rise * std::sqrt((k - 1) / (double)T));
                for (int d = std::min(dk, n_scan - 1); d < n_scan; d++)
                    slip_cap_arr[d]++;
            }
        }
        ms_idx = 0;
        nodes = 0;
        node_cap = cfg.node_cap;
        return true;
    }

    void dfs(int d, int cost, int cost_pre) {
        if (g_stop.load(std::memory_order_relaxed)) return;
        if (d == n_scan) {
            g_completions.fetch_add(1, std::memory_order_relaxed);
            report_completion(b);
            // anytime B&B: within this restart only accept strictly better
            budget = std::min(budget, cost - 1);
            return;
        }
        if (d > max_depth) {
            max_depth = d;
            // capture the whole prefix, not an increment: the path may have
            // backtracked through shallower cells since the last snapshot
            if (cfg.hybrid)
                for (int i = 0; i < d; i++) {
                    hyb_p[i] = b.piece[scan[i]];
                    hyb_r[i] = b.rot[scan[i]];
                }
        }
        int s = scan[d];
        int wcol = b.q(s - 1)[1];   // west neighbor's east side
        int ncol = b.q(s - W)[2];   // north neighbor's south side
        bool pre = !slip_on && (s / W) < free_row;
        int bb = bk.n_bands == 1 ? 0 : ZONE_BAND[s];

        auto try_list = [&](const std::vector<u32>& list, bool skip_exact_n,
                            bool skip_exact_w) {
            for (u32 v : list) {
                int cp = CP(v);
                if (used[cp]) continue;
                int pc = 0;
                if (grp_on) {
                    // piece-class schedule: spend useless pieces early, hold
                    // good (and especially precious) back for the tail
                    pc = pclass[cp];
                    if (pc != 3 && d < cfg.group_ck[2] &&
                        useless_left >= cfg.group_ck[2] - d) continue;
                    if (pc >= 1) {
                        if (d < cfg.group_ck[0]) continue;
                        if (d < cfg.group_ck[1] &&
                            good_used >= cfg.group_maxgood) continue;
                        if (pc == 2 && d < cfg.group_ck[3]) continue;
                    }
                }
                if (skip_exact_n && (int)CN(v) == ncol) continue;
                if (skip_exact_w && (int)CW(v) == wcol) continue;
                nodes++;
                if (ms_idx < (int)g_abort_ms.size() &&
                    nodes >= g_abort_ms[ms_idx].first) {
                    if (max_depth < g_abort_ms[ms_idx].second) {
                        // unproductive restart: unwind it entirely
                        g_aborts.fetch_add(1, std::memory_order_relaxed);
                        node_cap = 0;
                        return;
                    }
                    ms_idx++;
                }
                int c = place(s, cp, CR(v));
                if (cost + c + deficit > budget ||
                    (slip_on ? cost + c > slip_cap_arr[d]
                             : (pre && cost_pre + c > cfg.pre_budget))) {
                    unplace(s);
                    continue;
                }
                if (log_on) {
                    lp_cell[d] = (u16)s; lp_p[d] = (u8)cp; lp_r[d] = (u8)CR(v);
                    lp_w[d] = (u8)wcol; lp_n[d] = (u8)ncol;
                    if (d + 1 > snap_depth) {
                        snap_depth = d + 1;
                        memcpy(sp_cell, lp_cell, sizeof(u16) * (d + 1));
                        memcpy(sp_p, lp_p, d + 1);
                        memcpy(sp_r, lp_r, d + 1);
                        memcpy(sp_w, lp_w, d + 1);
                        memcpy(sp_n, lp_n, d + 1);
                    }
                }
                if (pc == 3) useless_left--; else if (pc >= 1) good_used++;
                dfs(d + 1, cost + c, cost_pre + (pre ? c : 0));
                if (pc == 3) useless_left++; else if (pc >= 1) good_used--;
                unplace(s);
                if (nodes > node_cap || g_stop.load(std::memory_order_relaxed)) return;
            }
        };

        const auto& exact = bk.exact[bb][wcol][ncol];
        try_list(exact, false, false);
        if (nodes > node_cap || g_stop.load(std::memory_order_relaxed)) return;
        // mismatch placements: in the prefix only as a fallback when no exact
        // candidate exists (keeps prefix branching tiny); free in the tail.
        // Under a slip schedule: allowed whenever this depth has headroom
        // (LV's integrated slipping -- no exact-empty gating).
        bool may_mismatch = slip_on ? cost < slip_cap_arr[d]
                          : pre    ? (cost_pre < cfg.pre_budget && exact.empty())
                                   : true;
        if (may_mismatch && budget - cost > deficit) {
            try_list(bk.by_w[bb][wcol], true, false);   // north mismatched
            if (nodes > node_cap) return;
            try_list(bk.by_n[bb][ncol], false, true);   // west mismatched
            // cost-2 tier: both W and N mismatch (skip flags remove every
            // candidate matching either side, leaving the double-break
            // complement). Opt-in for the hunter (--cost2 2): it widens tail
            // branching a lot, worth it only when the target family is known
            // to contain double-break cells.
            if (cfg.cost2 >= 2 && budget - cost > deficit + 1) {
                if (nodes > node_cap) return;
                try_list(bk.all_list[bb], true, true);
            }
        }
    }

    void run_restart() {
        if (!setup_restart()) return;
        max_depth = 0;
        snap_depth = 0;
        dfs(0, 0, 0);
        // hybrid completion: the DFS almost never completes a board, so its
        // deepest prefix used to be thrown away. Replay it and greedy-fill
        // the tail -> every restart emits a fresh-lineage completion
        // (perfect ring + ~perfect top rows + greedy bottom), typically well
        // above a from-scratch greedy_complete board.
        if (cfg.hybrid && max_depth > 0 &&
            !g_stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < max_depth; i++)
                place(scan[i], hyb_p[i], hyb_r[i]);
            greedy_fill(max_depth);
            g_hybrid.fetch_add(1, std::memory_order_relaxed);
            report_completion(b);
        }
        g_nodes.fetch_add(nodes, std::memory_order_relaxed);
        g_restarts.fetch_add(1, std::memory_order_relaxed);
        g_depth_sum.fetch_add((u64)max_depth, std::memory_order_relaxed);
        int md = max_depth, cur = g_depth_max.load(std::memory_order_relaxed);
        while (md > cur && !g_depth_max.compare_exchange_weak(cur, md)) {}
        if (log_on && snap_depth > 0) {
            std::lock_guard<std::mutex> lk(g_log_mu);
            g_log << snap_depth;
            for (int i = 0; i < snap_depth; i++)
                g_log << ' ' << sp_cell[i] << ':' << (int)sp_p[i] << ':'
                      << (int)sp_r[i] << ':' << (int)sp_w[i] << ':' << (int)sp_n[i];
            g_log << '\n';
        }
    }

    // greedy min-mismatch fill of scan[d0..n_scan) with random tie-break;
    // assumes cells before d0 are already placed. Always completes.
    int greedy_fill(int d0) {
        int total = 0;
        for (int d = d0; d < n_scan; d++) {
            int s = scan[d];
            int bestc = 1 << 20, bp = -1, bkr = 0, ties = 0;
            for (int p = 0; p < 256; p++) {
                if (used[p] || PTYPE[p] != 0) continue;
                for (int k = 0; k < 4; k++) {
                    const u8* q = QUAD[p][k];
                    int c = 0;
                    static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
                    for (int dir = 0; dir < 4; dir++) {
                        int ns = (s / W + DY[dir]) * W + s % W + DX[dir];
                        if (b.filled[ns] && b.q(ns)[(dir + 2) & 3] != q[dir]) c++;
                    }
                    if (c < bestc) { bestc = c; bp = p; bkr = k; ties = 1; }
                    else if (c == bestc && (int)rng.below((u32)++ties) == 0) { bp = p; bkr = k; }
                }
            }
            place(s, bp, bkr);
            total += bestc;
        }
        return total;
    }

    // fast full-board construction: min-mismatch piece at each cell, random
    // tie-break; always completes -> seeds the polisher pool
    void greedy_complete() {
        if (!setup_restart()) return;
        greedy_fill(0);
        g_completions.fetch_add(1, std::memory_order_relaxed);
        report_completion(b);
        g_restarts.fetch_add(1, std::memory_order_relaxed);
    }
};

// ------------------------------------------------------------------ band solver (macro-LNS: rebuild a horizontal
// band of rows exactly, holding everything else fixed). Modeled on Hunter's
// interior row-major DFS under a mismatch budget; no global mutable state.
struct BandSolver {
    Buckets bk;
    bool used[256] = {};
    int D[23] = {}, avail[23] = {};
    int deficit = 0;
    int scan[196], n_scan = 0;
    u64 nodes = 0, node_cap = 0;
    int budget = 0;
    int best_cost = 0;
    u8 best_piece[196], best_rot[196];  // indexed by scan position

    void bump_D(int c, int dv) {
        int before = std::max(0, D[c] - avail[c]);
        D[c] += dv;
        deficit += std::max(0, D[c] - avail[c]) - before;
    }
    void bump_avail(int c, int dv) {
        int before = std::max(0, D[c] - avail[c]);
        avail[c] += dv;
        deficit += std::max(0, D[c] - avail[c]) - before;
    }

    // place piece p rot k at cell s; cost = mismatches against neighbors that
    // are already decided (b.filled[]==true means fixed-or-earlier-in-scan,
    // since all free cells start cleared and are filled strictly in scan order)
    int place(Board& b, int s, int p, int k) {
        const u8* q = QUAD[p][k];
        b.piece[s] = (u8)p;
        b.rot[s] = (u8)k;
        b.filled[s] = true;
        used[p] = true;
        int cost = 0;
        static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
        for (int dir = 0; dir < 4; dir++) {
            int nx = s % W + DX[dir], ny = s / W + DY[dir];
            int ns = ny * W + nx;
            int opp = (dir + 2) & 3;
            if (b.filled[ns]) {
                u8 other = b.q(ns)[opp];
                if (other != q[dir]) cost++;
                bump_D(other, -1);
            } else {
                bump_D(q[dir], +1);
            }
        }
        for (int i = 0; i < 4; i++) bump_avail(q[i], -1);
        return cost;
    }
    void unplace(Board& b, int s) {
        int p = b.piece[s];
        const u8* q = b.q(s);
        for (int i = 0; i < 4; i++) bump_avail(q[i], +1);
        static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
        b.filled[s] = false;
        for (int dir = 0; dir < 4; dir++) {
            int ns = (s / W + DY[dir]) * W + s % W + DX[dir];
            int opp = (dir + 2) & 3;
            if (b.filled[ns]) bump_D(b.q(ns)[opp], +1);
            else bump_D(q[dir], -1);
        }
        used[p] = false;
    }

    void dfs(Board& b, int d, int cost) {
        if (g_stop.load(std::memory_order_relaxed)) return;
        if (d == n_scan) {
            if (cost < best_cost) {
                best_cost = cost;
                for (int i = 0; i < n_scan; i++) {
                    best_piece[i] = b.piece[scan[i]];
                    best_rot[i] = b.rot[scan[i]];
                }
                budget = cost - 1;
            }
            return;
        }
        int s = scan[d];
        int wcol = b.q(s - 1)[1];
        int ncol = b.q(s - W)[2];
        const auto& exact = bk.exact[0][wcol][ncol];
        auto try_list = [&](const std::vector<u32>& list, bool skip_exact_n,
                            bool skip_exact_w) {
            for (u32 v : list) {
                if (used[CP(v)]) continue;
                if (skip_exact_n && (int)CN(v) == ncol) continue;
                if (skip_exact_w && (int)CW(v) == wcol) continue;
                nodes++;
                int c = place(b, s, CP(v), CR(v));
                if (cost + c + deficit > budget) { unplace(b, s); continue; }
                dfs(b, d + 1, cost + c);
                unplace(b, s);
                if (nodes > node_cap || g_stop.load(std::memory_order_relaxed)) return;
            }
        };
        try_list(exact, false, false);
        if (nodes > node_cap || g_stop.load(std::memory_order_relaxed)) return;
        if (budget - cost > deficit) {
            try_list(bk.by_w[0][wcol], true, false);
            if (nodes > node_cap) return;
            try_list(bk.by_n[0][ncol], false, true);
            // cost-2 tier (default on here): band rebuilds around record
            // fault clusters must be able to re-place double-break cells or
            // the incumbent's own family is outside the search space.
            if (cfg.cost2 >= 1 && budget - cost > deficit + 1) {
                if (nodes > node_cap) return;
                try_list(bk.all_list[0], true, true);
            }
        }
    }

    // b must already be rotated to the working orientation. frozen[] marks
    // cells (in b's orientation) that must never move (transformed clue
    // cells). Returns the improvement (incumbent_cost - best_cost achieved),
    // 0 if no strict improvement was found; applies the best arrangement
    // in-place when improved, otherwise restores b exactly to its input state.
    int solve(Board& b, int y0, const bool* frozen, u64 cap, u64 seed) {
        bool in_free[NC] = {};
        n_scan = 0;
        for (int y = y0; y <= 14; y++)
            for (int x = 1; x <= 14; x++) {
                int s = y * W + x;
                if (!b.filled[s] || frozen[s]) continue;
                in_free[s] = true;
                scan[n_scan++] = s;
            }
        if (n_scan == 0) return 0;

        // incumbent cost: mismatched internal edges with >=1 endpoint free,
        // each edge counted once
        int inc_cost = 0;
        static const int DX2[4] = {0, 1, 0, -1}, DY2[4] = {-1, 0, 1, 0};
        for (int i = 0; i < n_scan; i++) {
            int s = scan[i], x = s % W, y = s / W;
            for (int dir = 0; dir < 4; dir++) {
                int nx = x + DX2[dir], ny = y + DY2[dir];
                int ns = ny * W + nx;
                if (in_free[ns] && ns < s) continue;  // count free-free once
                if (!b.filled[ns]) continue;
                u8 a = b.q(s)[dir], c = b.q(ns)[(dir + 2) & 3];
                if (!(a == c && a != 0)) inc_cost++;
            }
        }

        last_incumbent_cost = last_best_cost = inc_cost;
        last_n_free = n_scan;
        last_nodes = 0;
        if (inc_cost == 0) return 0;  // band already perfect

        // pool: pieces currently on free cells; remove them (clear filled),
        // remember original placement for restore
        u8 orig_piece[196], orig_rot[196];
        for (int i = 0; i < n_scan; i++) {
            orig_piece[i] = b.piece[scan[i]];
            orig_rot[i] = b.rot[scan[i]];
        }
        memset(used, 0, sizeof used);
        bool excl[256] = {};
        for (int p = 0; p < 256; p++) excl[p] = true;
        for (int i = 0; i < n_scan; i++) excl[orig_piece[i]] = false;
        for (int i = 0; i < n_scan; i++) b.filled[scan[i]] = false;

        memset(D, 0, sizeof D);
        memset(avail, 0, sizeof avail);
        deficit = 0;
        for (int p = 0; p < 256; p++)
            if (!excl[p] && PTYPE[p] == 0)
                for (int i = 0; i < 4; i++) avail[e2::PIECE_DEFS[p][i]]++;
        for (int i = 0; i < n_scan; i++) {
            int s = scan[i], x = s % W, y = s / W;
            for (int dir = 0; dir < 4; dir++) {
                int nx = x + DX2[dir], ny = y + DY2[dir];
                int ns = ny * W + nx;
                if (b.filled[ns]) bump_D(b.q(ns)[(dir + 2) & 3], +1);
            }
        }

        Rng rng(seed);
        bk.build(rng, excl);

        best_cost = inc_cost;  // budget=inc-1: only strictly better completions recorded
        budget = inc_cost - 1;
        nodes = 0;
        node_cap = cap;
        for (int i = 0; i < n_scan; i++) {
            best_piece[i] = orig_piece[i];
            best_rot[i] = orig_rot[i];
        }
        dfs(b, 0, 0);

        int improvement = best_cost < inc_cost ? inc_cost - best_cost : 0;
        // apply best (== incumbent if nothing better was found)
        for (int i = 0; i < n_scan; i++) {
            b.piece[scan[i]] = best_piece[i];
            b.rot[scan[i]] = best_rot[i];
            b.filled[scan[i]] = true;
        }
        last_best_cost = std::min(best_cost, inc_cost);
        last_nodes = nodes;
        return improvement;
    }

    // stats from the last solve() call, for --mode band reporting
    int last_incumbent_cost = 0, last_best_cost = 0, last_n_free = 0;
    u64 last_nodes = 0;
};

static void hungarian(int n, const int* cost, int* row_of_col);  // defined below

// ------------------------------------------------------------------ region solver (exact B&B, used by polisher)
struct RegionSolver {
    // cells to fill (scan order), pieces available (indices), board with cells cleared
    Board* b;
    int cells[40], n_cells;
    int pieces[40], n_pieces;
    bool pused[40];
    int best_gain;                 // best score found so far (edges incident to region)
    u8 best_piece[40], best_rot[40];
    int decided[40];               // #edges decided when filling cell i (to fixed or earlier region cells)
    int suffix_max[41];
    // dynamic suffix bound: per-cell best/second-best piece value and the
    // argmax piece index. When the argmax piece is already used deeper in the
    // tree, that cell's contribution drops to the second-best -- a strictly
    // tighter (still valid) upper bound than the static suffix_max.
    int cmax1[40], cmax2[40], amax_pi[40];
    u64 nodes, cap;
    Rng rrng{1};                   // tie-break randomization (sideways drift)

    // count edge match: returns 1 if colors equal and non-grey
    static inline int em(u8 a, u8 c) { return a == c && a != 0; }

    // seed_incumbent=true (default, all pre-existing call sites): B&B starts
    // with the original arrangement as initial best, so capped searches can
    // never lose ground (returns max(best_gain, inc_gain)). seed_incumbent=
    // false (non-seeded exploration, task #4 Part B): best_gain starts at -1
    // and the incumbent is NOT pre-loaded as best, so the returned gain can
    // be worse than inc_gain -- callers using this mode must gate acceptance
    // themselves (e.g. via LAHC) and be prepared to restore the pre-move
    // region arrangement on reject.
    int prep_and_solve(Board& board, const int* cs, int n, u64 node_cap, u64 seed,
                        bool seed_incumbent = true) {
        b = &board;
        n_cells = n;
        for (int i = 0; i < n; i++) cells[i] = cs[i];
        std::sort(cells, cells + n);
        n_pieces = n;
        for (int i = 0; i < n; i++) {
            pieces[i] = board.piece[cells[i]];
            board.filled[cells[i]] = false;
            pused[i] = false;
        }
        // most-constrained-first: sort by number of fixed (non-region)
        // neighbors descending, ties row-major; cuts branching a lot
        {
            int fixedn[40];
            for (int i = 0; i < n; i++) {
                int s = cells[i], x = s % W, y = s / W, fc = 0;
                static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
                for (int dir = 0; dir < 4; dir++) {
                    int nx = x + DX[dir], ny = y + DY[dir];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    if (board.filled[ny * W + nx]) fc++;
                }
                fixedn[i] = fc;
            }
            int idx[40];
            for (int i = 0; i < n; i++) idx[i] = i;
            std::sort(idx, idx + n, [&](int a2, int b2) {
                if (fixedn[a2] != fixedn[b2]) return fixedn[a2] > fixedn[b2];
                return cells[a2] < cells[b2];
            });
            int tmp[40];
            for (int i = 0; i < n; i++) tmp[i] = cells[idx[i]];
            for (int i = 0; i < n; i++) cells[i] = tmp[i];
        }
        // decided-edge counts for bound, split into fixed vs region-internal
        int internal_cnt[40];
        for (int i = 0; i < n; i++) {
            int s = cells[i], x = s % W, y = s / W;
            int dcount = 0, fcount = 0;
            static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
            for (int dir = 0; dir < 4; dir++) {
                int nx = x + DX[dir], ny = y + DY[dir];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                int ns = ny * W + nx;
                if (board.filled[ns]) { dcount++; fcount++; }  // fixed neighbor
                else {
                    // earlier region cell?
                    for (int j = 0; j < i; j++)
                        if (cells[j] == ns) { dcount++; break; }
                }
            }
            decided[i] = dcount;
            internal_cnt[i] = dcount - fcount;
        }
        // per-(cell,piece) upper bound: best-rotation fixed-edge matches +
        // all internal decided edges assumed matched. val[i][pi] = -1 when
        // the piece cannot legally sit on the cell (type / ring rotation).
        // cmax1[i] tightens suffix_max vs decided[i], which assumed any
        // piece matches every decided edge.
        int val[40][40];
        for (int i = 0; i < n; i++) {
            int s = cells[i], x = s % W, y = s / W;
            int ct = CELL_TYPE[s];
            static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
            for (int pi = 0; pi < n; pi++) {
                int p = pieces[pi], v = -1;
                if (PTYPE[p] == (ct == 0 ? 0 : ct)) {
                    int k0 = 0, k1 = 3;
                    if (ct != 0) {
                        int k = ring_rot_for_cell(s, p);
                        if (k < 0) { val[i][pi] = -1; continue; }
                        k0 = k1 = k;
                    }
                    for (int k = k0; k <= k1; k++) {
                        const u8* q = QUAD[p][k];
                        int fm = 0;
                        for (int dir = 0; dir < 4; dir++) {
                            int nx = x + DX[dir], ny = y + DY[dir];
                            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                            int ns = ny * W + nx;
                            if (board.filled[ns]) fm += em(q[dir], board.q(ns)[(dir + 2) & 3]);
                        }
                        if (fm > v) v = fm;
                    }
                    if (v >= 0) v += internal_cnt[i];
                }
                val[i][pi] = v;
            }
        }
        // top-2 per cell for the dynamic suffix bound (clamped at 0: an
        // infeasible cell just weakens the bound, correctness is unaffected)
        for (int i = 0; i < n; i++) {
            int m1 = 0, m2 = 0, ap = 0;
            for (int pi = 0; pi < n; pi++) {
                int v = val[i][pi];
                if (v > m1) { m2 = m1; m1 = v; ap = pi; }
                else if (v > m2) m2 = v;
            }
            cmax1[i] = m1;
            cmax2[i] = m2;
            amax_pi[i] = ap;
        }
        suffix_max[n] = 0;
        for (int i = n - 1; i >= 0; i--) suffix_max[i] = suffix_max[i + 1] + cmax1[i];
        rrng.s = seed;
        // compute the incumbent (original arrangement) gain regardless of
        // seed_incumbent: needed both to seed best_gain (seeded mode) and to
        // report a comparable return value (non-seeded mode)
        int inc_gain = 0;
        for (int i = 0; i < n; i++) {
            best_piece[i] = board.piece[cells[i]];
            best_rot[i] = board.rot[cells[i]];
        }
        {
            static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
            for (int i = 0; i < n; i++) {
                int s = cells[i], x = s % W, y = s / W;
                const u8* q = QUAD[best_piece[i]][best_rot[i]];
                for (int dir = 0; dir < 4; dir++) {
                    int nx = x + DX[dir], ny = y + DY[dir];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    int ns = ny * W + nx;
                    if (board.filled[ns]) inc_gain += em(q[dir], board.q(ns)[(dir + 2) & 3]);
                    else for (int j = 0; j < i; j++)
                        if (cells[j] == ns) {
                            inc_gain += em(q[dir], QUAD[best_piece[j]][best_rot[j]][(dir + 2) & 3]);
                            break;
                        }
                }
            }
        }
        if (seed_incumbent) {
            // seed the search with the incumbent: capped searches can then
            // never lose ground, and the strong initial lower bound prunes
            // most of the tree from the start. Ties may still replace the
            // incumbent (drift).
            best_gain = inc_gain - 1;
        } else {
            // non-seeded exploration (task #4 Part B): do not pre-load the
            // incumbent as best, so the search is free to land on a worse
            // arrangement than it started with. best_piece/best_rot above
            // still hold the incumbent as a placeholder in case rec() never
            // improves on best_gain=-1 (it always will for n>=1, since any
            // full assignment has gain>=0), but that placeholder is
            // overwritten as soon as rec() finds any complete assignment.
            best_gain = -1;
        }
        nodes = 0;
        cap = node_cap;
        // root assignment relaxation (Hungarian, O(n^3)): amax is an upper
        // bound on any arrangement's gain, ignoring only internal-edge
        // consistency between region cells. amax >= inc_gain always (the
        // incumbent witnesses it). If amax == inc_gain, no strict improvement
        // exists in this region -- the search is then useful only for
        // sideways drift, so run it at a fraction of the node budget and
        // spend the savings on regions that can actually gain.
        if (seed_incumbent && n >= 6) {
            const int BIG = 8;
            int cost[40 * 40], row_of_col[40];
            for (int i = 0; i < n; i++)
                for (int pi = 0; pi < n; pi++)
                    cost[i * n + pi] = BIG - val[i][pi];
            hungarian(n, cost, row_of_col);
            int amax = 0;
            for (int j = 0; j < n; j++) amax += val[row_of_col[j]][j];
            if (amax <= inc_gain) {
                cap = node_cap / 16;
                g_region_tieproof.fetch_add(1, std::memory_order_relaxed);
            }
        }
        rec(0, 0);
        // apply best
        for (int i = 0; i < n; i++) {
            board.piece[cells[i]] = best_piece[i];
            board.rot[cells[i]] = best_rot[i];
            board.filled[cells[i]] = true;
        }
        return seed_incumbent ? std::max(best_gain, inc_gain) : best_gain;
    }

    void rec(int i, int gain) {
        if (i == n_cells) {
            if (gain > best_gain) {
                best_gain = gain;
                for (int k = 0; k < n_cells; k++) {
                    best_piece[k] = b->piece[cells[k]];
                    best_rot[k] = b->rot[cells[k]];
                }
            }
            return;
        }
        if (gain + suffix_max[i] <= best_gain) return;   // cheap static bound
        // dynamic bound: cells whose argmax piece is already used contribute
        // only their second-best value. Always <= suffix_max[i], often
        // strictly, which is what makes region_max_big windows tractable.
        int sfx = 0;
        for (int j = i; j < n_cells; j++)
            sfx += pused[amax_pi[j]] ? cmax2[j] : cmax1[j];
        if (gain + sfx <= best_gain) return;
        if (nodes > cap) return;
        int s = cells[i];
        int ct = CELL_TYPE[s];
        static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
        // gather candidates, try highest immediate gain first: strong
        // completions found early tighten the bound sooner
        struct Cand { u8 pi, k, g, r; };
        Cand cand[160];
        int nc = 0;
        for (int pi = 0; pi < n_pieces; pi++) {
            if (pused[pi]) continue;
            int p = pieces[pi];
            if (PTYPE[p] != (ct == 0 ? 0 : ct)) continue;
            int k0 = 0, k1 = 3;
            if (ct != 0) {
                int k = ring_rot_for_cell(s, p);
                if (k < 0) continue;
                k0 = k1 = k;
            }
            for (int k = k0; k <= k1; k++) {
                const u8* q = QUAD[p][k];
                int g = 0;
                for (int dir = 0; dir < 4; dir++) {
                    int nx = s % W + DX[dir], ny = s / W + DY[dir];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    int ns = ny * W + nx;
                    if (b->filled[ns]) g += em(q[dir], b->q(ns)[(dir + 2) & 3]);
                }
                cand[nc++] = {(u8)pi, (u8)k, (u8)g, (u8)(rrng.next() >> 56)};
            }
        }
        std::sort(cand, cand + nc, [](const Cand& a, const Cand& c) {
            return a.g != c.g ? a.g > c.g : a.r < c.r;
        });
        // dynamic suffix for cells after this one (pused unchanged inside the
        // loop until place, and a placed candidate can only shrink the true
        // bound further -- so this stays a valid upper bound per candidate)
        int sfx1 = sfx - (pused[amax_pi[i]] ? cmax2[i] : cmax1[i]);
        for (int ci = 0; ci < nc; ci++) {
            // candidates sorted by g desc: once one is prunable, all are
            if (gain + cand[ci].g + sfx1 <= best_gain) break;
            nodes++;
            int pi = cand[ci].pi;
            b->piece[s] = (u8)pieces[pi];
            b->rot[s] = cand[ci].k;
            b->filled[s] = true;
            pused[pi] = true;
            rec(i + 1, gain + cand[ci].g);
            pused[pi] = false;
            b->filled[s] = false;
            if (nodes > cap) return;
        }
    }
};

// ------------------------------------------------------------------ hungarian (min-cost perfect assignment, O(n^3))
// cost is n x n (row-major); fills row_of_col[j] = row assigned to column j
static void hungarian(int n, const int* cost, int* row_of_col) {
    const int INF = 1 << 28;
    std::vector<int> u(n + 1, 0), v(n + 1, 0), p(n + 1, 0), way(n + 1, 0);
    for (int i = 1; i <= n; i++) {
        p[0] = i;
        int j0 = 0;
        std::vector<int> minv(n + 1, INF);
        std::vector<char> used(n + 1, 0);
        do {
            used[j0] = 1;
            int i0 = p[j0], delta = INF, j1 = 0;
            for (int j = 1; j <= n; j++)
                if (!used[j]) {
                    int cur = cost[(i0 - 1) * n + (j - 1)] - u[i0] - v[j];
                    if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                    if (minv[j] < delta) { delta = minv[j]; j1 = j; }
                }
            for (int j = 0; j <= n; j++)
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else minv[j] -= delta;
            j0 = j1;
        } while (p[j0] != 0);
        do { int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0);
    }
    for (int j = 1; j <= n; j++) row_of_col[j - 1] = p[j] - 1;
}

// ------------------------------------------------------------------ polisher
struct Polisher {
    Rng rng;
    Board cur;
    int cur_score = -1;
    RegionSolver rs;
    BandSolver bands;
    int since_gain = 0;
    // Track B: restrict LNS to a cell mask (thin subgrid wrapper). Piece-pool
    // restriction is automatic: both LNS moves only permute pieces already on
    // the selected cells. local_only additionally detaches the polisher from
    // the global pools/best so it can work on a partial (quadrant) board.
    const bool* mask = nullptr;
    int mx0 = 0, my0 = 0, mx1 = W - 1, my1 = H - 1;  // rectangle window bounds
    bool local_only = false;

    // LAHC (late acceptance hill-climbing) gate for non-seeded exploration
    // moves (task #4 Part B): per-thread circular history of accepted full
    // board scores. cfg.lahc_len==0 disables both LAHC and exploration moves
    // (region moves always run incumbent-seeded, as before).
    std::vector<int> lahc_hist;
    int lahc_i = 0;
    bool lahc_init = false;

    // plateau-merge drift snapshots (task #5 Part A): identifies this
    // polisher's drift file when cfg.drift_dir is set. Set by the thread
    // launcher (mix_thread/polisher_thread loop index); irrelevant when the
    // feature is off.
    int thread_id = 0;
    // once-per-arrival latch for the "reached g_best_score" snapshot so a
    // polisher sitting at the best score doesn't spam the file every step.
    int drift_last_best_seen = -1;

    explicit Polisher(u64 seed) : rng(seed) {}

    void lahc_ensure() {
        if (lahc_init) return;
        int len = std::max(1, cfg.lahc_len);
        lahc_hist.assign(len, cur_score);
        lahc_i = 0;
        lahc_init = true;
    }

    bool ensure_board() {
        if (cur_score >= 0) return true;
        Board b;
        // 30%: fresh lineage (any completion) so polishers are not trapped
        // forever in the incumbent's basin; else elite pool, else best
        bool got = (rng.below(100) < 30 && g_fresh.get(rng, b)) ||
                   g_seeds.get(rng, b) || g_fresh.get(rng, b);
        if (!got) {
            std::lock_guard<std::mutex> lk(g_best_mu);
            if (g_best_score.load() < 0) return false;
            b = g_best_board;
        }
        cur = b;
        cur_score = cur.score();
        // fresh board -> stale LAHC history would gate acceptance against an
        // unrelated lineage's scores; reseed lazily from this board's score
        // at the next explore move (lahc_ensure)
        lahc_init = false;
        return true;
    }

    // collect cells touching a mismatched edge (excluding clue cells);
    // edges with an unfilled endpoint are unconstrained, not faults
    int fault_cells(int* out) {
        bool mark[NC] = {};
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int s = y * W + x;
                if (!cur.filled[s]) continue;
                if (x + 1 < W && cur.filled[s + 1]) {
                    u8 a = cur.q(s)[1], b2 = cur.q(s + 1)[3];
                    if (!(a == b2 && a != 0)) mark[s] = mark[s + 1] = true;
                }
                if (y + 1 < H && cur.filled[s + W]) {
                    u8 a = cur.q(s)[2], b2 = cur.q(s + W)[0];
                    if (!(a == b2 && a != 0)) mark[s] = mark[s + W] = true;
                }
            }
        int n = 0;
        for (int s = 0; s < NC; s++)
            if (mark[s] && !IS_CLUE_CELL[s] && (!mask || mask[s])) out[n++] = s;
        return n;
    }

    // pick region cells for B&B re-solve: rectangle or fault blob.
    // cap = max cells (region_max, or region_max_big on big-region rolls)
    int pick_region(int* out, int cap) {
        int roll = (int)rng.below(100);
        if (roll < 50) {  // rectangle window within [mx0..mx1]x[my0..my1]
            int bw = mx1 - mx0 + 1, bh = my1 - my0 + 1;
            int smax = std::min(8, 2 + cap / 5);  // 5 at cap=16 (old behavior)
            int w = 2 + (int)rng.below((u32)(smax - 1)),
                h = 2 + (int)rng.below((u32)(smax - 1));
            if (w > bw) w = bw;
            if (h > bh) h = bh;
            while (w * h > cap) (rng.below(2) && w > 2 ? w : h)--;
            int x0 = mx0 + (int)rng.below((u32)(bw - w + 1));
            int y0 = my0 + (int)rng.below((u32)(bh - h + 1));
            int n = 0;
            for (int y = y0; y < y0 + h; y++)
                for (int x = x0; x < x0 + w; x++) {
                    int s = y * W + x;
                    if (!IS_CLUE_CELL[s] && cur.filled[s] && (!mask || mask[s]))
                        out[n++] = s;
                }
            return n;
        }
        // blob grown from a random conflicted edge
        int confl[480][2], nc = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int s = y * W + x;
                if (!cur.filled[s]) continue;
                if (x + 1 < W && cur.filled[s + 1]) {
                    u8 a = cur.q(s)[1], b2 = cur.q(s + 1)[3];
                    if (!(a == b2 && a != 0)) { confl[nc][0] = s; confl[nc][1] = s + 1; nc++; }
                }
                if (y + 1 < H && cur.filled[s + W]) {
                    u8 a = cur.q(s)[2], b2 = cur.q(s + W)[0];
                    if (!(a == b2 && a != 0)) { confl[nc][0] = s; confl[nc][1] = s + W; nc++; }
                }
            }
        if (nc == 0) return 0;
        int n = 0;
        int pickc = (int)rng.below((u32)nc);
        bool inr[NC] = {};
        int queue[NC], qn = 0;
        for (int j = 0; j < 2; j++) {
            int s = confl[pickc][j];
            if (!IS_CLUE_CELL[s] && !inr[s] && (!mask || mask[s])) {
                inr[s] = true; queue[qn++] = s; out[n++] = s;
            }
        }
        if (n == 0) return 0;
        int target = 6 + (int)rng.below((u32)(cap - 6));
        while (n < target && qn > 0) {
            int qi = (int)rng.below((u32)qn);
            int s = queue[qi];
            static const int DD[4] = {-W, 1, W, -1};
            int dir = (int)rng.below(4);
            bool grew = false;
            for (int t = 0; t < 4 && !grew; t++, dir = (dir + 1) & 3) {
                int x = s % W, y = s / W;
                if ((dir == 1 && x == W - 1) || (dir == 3 && x == 0) ||
                    (dir == 0 && y == 0) || (dir == 2 && y == H - 1)) continue;
                int ns = s + DD[dir];
                if (inr[ns] || IS_CLUE_CELL[ns] || !cur.filled[ns] ||
                    (mask && !mask[ns])) continue;
                inr[ns] = true;
                queue[qn++] = ns;
                out[n++] = ns;
                grew = true;
            }
            if (!grew) { queue[qi] = queue[--qn]; }
        }
        return n;
    }

    // grow a random connected blob of ~cfg.umbrella_dist cells (jittered
    // 25-40), BFS from a random non-clue filled cell. Like pick_region's
    // blob branch but: (1) starts from an arbitrary filled cell, not a
    // conflicted edge, so it can uproot healthy regions too, and (2) has no
    // region_max cap -- the whole point is a large, disruptive jump.
    int pick_umbrella_blob(int* out) {
        int filledc[NC], nf = 0;
        for (int s = 0; s < NC; s++)
            if (cur.filled[s] && !IS_CLUE_CELL[s] && (!mask || mask[s])) filledc[nf++] = s;
        if (nf == 0) return 0;
        int start = filledc[rng.below((u32)nf)];
        int n = 0;
        bool inr[NC] = {};
        int queue[NC], qn = 0;
        inr[start] = true;
        queue[qn++] = start;
        out[n++] = start;
        // jitter +/-7 around cfg.umbrella_dist (default 32 -> range 25-40 per
        // spec); --umbrella-dist shifts the jitter window, still clamped to
        // a sane [2, NC] span
        int center = std::max(1, cfg.umbrella_dist);
        int lo = std::max(2, center - 7), hi = center + 8;
        int target = lo + (int)rng.below((u32)(hi - lo + 1));
        target = std::min(target, nf);  // can't exceed available (non-clue) cells
        while (n < target && qn > 0) {
            int qi = (int)rng.below((u32)qn);
            int s = queue[qi];
            static const int DD[4] = {-W, 1, W, -1};
            int dir = (int)rng.below(4);
            bool grew = false;
            for (int t = 0; t < 4 && !grew; t++, dir = (dir + 1) & 3) {
                int x = s % W, y = s / W;
                if ((dir == 1 && x == W - 1) || (dir == 3 && x == 0) ||
                    (dir == 0 && y == 0) || (dir == 2 && y == H - 1)) continue;
                int ns = s + DD[dir];
                if (inr[ns] || IS_CLUE_CELL[ns] || !cur.filled[ns] ||
                    (mask && !mask[ns])) continue;
                inr[ns] = true;
                queue[qn++] = ns;
                out[n++] = ns;
                grew = true;
            }
            if (!grew) { queue[qi] = queue[--qn]; }
        }
        return n;
    }

    // plateau-merge drift snapshot (task #5 Part A): append one line
    // "<score> <1024-char edges>" to <drift_dir>/drift_t<thread_id>.txt,
    // reusing Board::edges_string() (the same board->edges idiom as
    // save_board/report_completion). Capped at 64 lines/file (drop oldest);
    // simplest correct implementation is an in-memory deque rewritten to disk
    // on every append -- these fire rarely (once per stagnation cycle, or
    // once per best-score arrival), so the O(64) rewrite cost is negligible.
    // Thread-local file (one Polisher instance per OS thread): no locking.
    void drift_snapshot(int score) {
        if (cfg.drift_dir.empty()) return;
        std::string path = cfg.drift_dir + "/drift_t" + std::to_string(thread_id) + ".txt";
        std::deque<std::string> lines;
        {
            std::ifstream in(path);
            std::string line;
            while (std::getline(in, line))
                if (!line.empty()) lines.push_back(line);
        }
        char buf[16];
        snprintf(buf, sizeof buf, "%d ", score);
        lines.push_back(std::string(buf) + cur.edges_string());
        while (lines.size() > 64) lines.pop_front();
        std::ofstream out(path, std::ios::trunc);
        for (auto& l : lines) out << l << "\n";
    }

    // umbrella distance-kick (task #4 Part A, from the 5-clue record
    // author's method): on deep stagnation, clear a large random blob and
    // rebuild it greedily (min-immediate-mismatch per cell, mirroring
    // Hunter::greedy_complete but restricted to the blob's freed piece
    // pool), then accept UNCONDITIONALLY -- the point is to leave the
    // current solution's basin of attraction even though the score will
    // usually drop. Never touches g_seeds/g_fresh/g_best: those are only
    // ever populated through report_completion / pool .add() calls, and this
    // function makes neither.
    void umbrella_jump() {
        int cells[NC];
        int n = pick_umbrella_blob(cells);
        if (n < 2) return;
        // freed piece pool = pieces currently occupying the blob cells
        bool avail_p[256] = {};
        for (int i = 0; i < n; i++) avail_p[cur.piece[cells[i]]] = true;
        for (int i = 0; i < n; i++) cur.filled[cells[i]] = false;
        // row-major order within the blob (matches greedy_complete's scan
        // discipline: fill in a fixed order so "already decided" neighbors
        // are well-defined at each step)
        std::sort(cells, cells + n);
        static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
        for (int i = 0; i < n; i++) {
            int s = cells[i];
            int bestc = 1 << 20, bp = -1, bkr = 0, ties = 0;
            for (int p = 0; p < 256; p++) {
                if (!avail_p[p]) continue;
                for (int k = 0; k < 4; k++) {
                    const u8* q = QUAD[p][k];
                    int c = 0;
                    for (int dir = 0; dir < 4; dir++) {
                        int nx = s % W + DX[dir], ny = s / W + DY[dir];
                        if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                        int ns = ny * W + nx;
                        if (cur.filled[ns] && cur.q(ns)[(dir + 2) & 3] != q[dir]) c++;
                    }
                    if (c < bestc) { bestc = c; bp = p; bkr = k; ties = 1; }
                    else if (c == bestc && (int)rng.below((u32)++ties) == 0) { bp = p; bkr = k; }
                }
            }
            avail_p[bp] = false;
            cur.piece[s] = (u8)bp;
            cur.rot[s] = (u8)bkr;
            cur.filled[s] = true;
        }
        cur_score = cur.score();
        g_umbrella_jumps.fetch_add(1, std::memory_order_relaxed);
        since_gain = 0;
        // deliberately NOT pushed to g_seeds/g_fresh/g_best here: the board
        // just got worse on purpose. Later region/scatter/band moves that
        // improve it will report it through the normal score-gated paths.
    }

    // optimal reassignment of pairwise non-adjacent interior fault cells
    // (assignment problem; the previous arrangement is feasible, so the
    // optimum can only match or improve the incident score)
    void scatter_move() {
        int fc[NC];
        int nf = fault_cells(fc);
        // shuffle, then greedily keep non-adjacent interior cells
        for (int i = nf - 1; i > 0; i--) std::swap(fc[i], fc[rng.below((u32)(i + 1))]);
        int cells[48], n = 0;
        bool picked[NC] = {};
        for (int i = 0; i < nf && n < cfg.scatter_max && n < 48; i++) {
            int s = fc[i];
            if (CELL_TYPE[s] != 0) continue;
            if (picked[s - 1] || picked[s + 1] || picked[s - W] || picked[s + W]) continue;
            picked[s] = true;
            cells[n++] = s;
        }
        if (n < 2) return;
        g_scatter_iters.fetch_add(1, std::memory_order_relaxed);

        // weight = best matches over rotations vs the 4 fixed neighbors
        static thread_local std::vector<int> wmat, cost;
        static thread_local std::vector<u8> brot;
        wmat.assign(n * n, 0);
        cost.assign(n * n, 0);
        brot.assign(n * n, 0);
        int before = 0;
        u8 nbr[48][4];  // required color per direction for each cell
        for (int j = 0; j < n; j++) {
            int s = cells[j];
            // unfilled neighbor -> 0: never counts as a match either way
            nbr[j][0] = cur.filled[s - W] ? cur.q(s - W)[2] : 0;
            nbr[j][1] = cur.filled[s + 1] ? cur.q(s + 1)[3] : 0;
            nbr[j][2] = cur.filled[s + W] ? cur.q(s + W)[0] : 0;
            nbr[j][3] = cur.filled[s - 1] ? cur.q(s - 1)[1] : 0;
            const u8* q = cur.q(s);
            for (int dir = 0; dir < 4; dir++)
                if (q[dir] == nbr[j][dir] && q[dir] != 0) before++;
        }
        for (int i = 0; i < n; i++) {
            int p = cur.piece[cells[i]];
            for (int j = 0; j < n; j++) {
                int bw = -1, bk = 0;
                for (int k = 0; k < 4; k++) {
                    const u8* q = QUAD[p][k];
                    int m = 0;
                    for (int dir = 0; dir < 4; dir++)
                        if (q[dir] == nbr[j][dir] && q[dir] != 0) m++;
                    if (m > bw) { bw = m; bk = k; }
                }
                wmat[i * n + j] = bw;
                brot[i * n + j] = (u8)bk;
                cost[i * n + j] = 4 - bw;
            }
        }
        int row_of_col[48];
        hungarian(n, cost.data(), row_of_col);
        int after = 0;
        for (int j = 0; j < n; j++) after += wmat[row_of_col[j] * n + j];
        if (dbg) printf("scatter n=%d before=%d after=%d\n", n, before, after);
        if (after > before || (after == before && (int)rng.below(100) < cfg.tie_pct)) {
            u8 newp[48];
            for (int j = 0; j < n; j++) newp[j] = cur.piece[cells[row_of_col[j]]];
            for (int j = 0; j < n; j++) {
                int i = row_of_col[j];
                cur.piece[cells[j]] = newp[j];
                cur.rot[cells[j]] = brot[i * n + j];
            }
            if (after > before) {
                cur_score += after - before;
                g_scatter_gains.fetch_add(1, std::memory_order_relaxed);
                g_polish_gains.fetch_add(1, std::memory_order_relaxed);
                since_gain = 0;
                if (local_only) return;
                if (cur_score > g_best_score.load(std::memory_order_relaxed))
                    report_completion(cur);
                else if (cur_score + cfg.seed_slack >= g_best_score.load())
                    g_seeds.add(cur);
            }
        }
    }

    // exact optimal reassignment of an entire chessboard-parity class of
    // interior cells: same-parity cells are pairwise non-adjacent, so every
    // neighbor stays fixed during the move and the ~97-cell simultaneous
    // reassignment is a pure assignment problem, solved exactly in O(n^3) --
    // coverage no <=16-cell region window or 48-cell scatter can reach.
    // Costs carry a small random perturbation (total < one match unit) so
    // ties break randomly: equal-score optima become free sideways drift.
    void parity_move() {
        int par = (int)rng.below(2);
        int cells[112], n = 0;
        for (int y = 1; y <= H - 2; y++)
            for (int x = 2 - ((par + y) & 1); x <= W - 2; x += 2) {
                int s = y * W + x;
                if (IS_CLUE_CELL[s] || !cur.filled[s]) continue;
                if (mask && !mask[s]) continue;
                cells[n++] = s;
            }
        if (n < 2) return;
        g_parity_iters.fetch_add(1, std::memory_order_relaxed);

        static thread_local std::vector<int> wmat, cost;
        static thread_local std::vector<u8> brot;
        wmat.assign(n * n, 0);
        cost.assign(n * n, 0);
        brot.assign(n * n, 0);
        // SCALE = one match unit; per-entry noise < SCALE/n keeps the noise
        // total below one unit, so the perturbed optimum is still a
        // match-count optimum. Small SCALE keeps Hungarian potentials far
        // from its INF sentinel.
        constexpr int SCALE = 1 << 14;
        const u32 pert = (u32)(SCALE / (n + 1));
        int before = 0;
        u8 nbr[112][4];  // required color per direction; unfilled -> 0 (never matches)
        for (int j = 0; j < n; j++) {
            int s = cells[j];
            nbr[j][0] = cur.filled[s - W] ? cur.q(s - W)[2] : 0;
            nbr[j][1] = cur.filled[s + 1] ? cur.q(s + 1)[3] : 0;
            nbr[j][2] = cur.filled[s + W] ? cur.q(s + W)[0] : 0;
            nbr[j][3] = cur.filled[s - 1] ? cur.q(s - 1)[1] : 0;
            const u8* q = cur.q(s);
            for (int dir = 0; dir < 4; dir++)
                if (q[dir] == nbr[j][dir] && q[dir] != 0) before++;
        }
        for (int i = 0; i < n; i++) {
            int p = cur.piece[cells[i]];
            for (int j = 0; j < n; j++) {
                int bw = -1, bk = 0;
                for (int k = 0; k < 4; k++) {
                    const u8* q = QUAD[p][k];
                    int m = 0;
                    for (int dir = 0; dir < 4; dir++)
                        if (q[dir] == nbr[j][dir] && q[dir] != 0) m++;
                    if (m > bw) { bw = m; bk = k; }
                }
                wmat[i * n + j] = bw;
                brot[i * n + j] = (u8)bk;
                cost[i * n + j] = (4 - bw) * SCALE + (int)rng.below(pert);
            }
        }
        int row_of_col[112];
        hungarian(n, cost.data(), row_of_col);
        int after = 0;
        for (int j = 0; j < n; j++) after += wmat[row_of_col[j] * n + j];
        if (dbg) printf("parity par=%d n=%d before=%d after=%d\n", par, n, before, after);
        // the incumbent assignment is feasible, so after >= before always
        if (after > before || (after == before && (int)rng.below(100) < cfg.tie_pct)) {
            u8 newp[112];
            for (int j = 0; j < n; j++) newp[j] = cur.piece[cells[row_of_col[j]]];
            for (int j = 0; j < n; j++) {
                int i = row_of_col[j];
                cur.piece[cells[j]] = newp[j];
                cur.rot[cells[j]] = brot[i * n + j];
            }
            if (after > before) {
                cur_score += after - before;
                g_parity_gains.fetch_add(1, std::memory_order_relaxed);
                g_polish_gains.fetch_add(1, std::memory_order_relaxed);
                since_gain = 0;
                if (local_only) return;
                if (cur_score > g_best_score.load(std::memory_order_relaxed))
                    report_completion(cur);
                else if (cur_score + cfg.seed_slack >= g_best_score.load())
                    g_seeds.add(cur);
            }
        }
    }

    // exact plateau merge (in-process plateau_merge.py): draw two archived
    // near-best boards, compute their disagreement cells, and re-solve that
    // window exactly on the better board. Outside the window the boards
    // agree, so the window piece multisets are identical -- the B&B search
    // space contains both parents' arrangements (and everything between).
    // Incumbent-seeded: can only gain or drift. Cross-basin pairs (~250
    // disagreements) are rejected as soon as the count passes the array cap.
    void merge_move() {
        Board X, Y;
        if (!g_plateau.get(rng, X) || !g_plateau.get(rng, Y)) return;
        int gb = g_best_score.load(std::memory_order_relaxed);
        int sx = X.score(), sy = Y.score();
        if (sx + 1 < gb || sy + 1 < gb) return;  // stale archive entries
        if (sy > sx) std::swap(X, Y);
        int cells[40], nd = 0;
        for (int s = 0; s < NC; s++) {
            if (X.piece[s] == Y.piece[s] && X.rot[s] == Y.rot[s]) continue;
            if (IS_CLUE_CELL[s] || nd >= 40) return;  // cross-basin: too far apart
            cells[nd++] = s;
        }
        if (nd < 2) return;  // identical boards
        {
            u64 ph = board_hash(X) ^ board_hash(Y);  // symmetric pair id
            std::lock_guard<std::mutex> lk(g_merge_mu);
            if (g_merge_tried.size() > (1u << 20)) g_merge_tried.clear();
            if (!g_merge_tried.insert(ph).second) return;  // pair already solved
        }
        g_merge_iters.fetch_add(1, std::memory_order_relaxed);
        auto [before, total_edges] = incident_score_and_edges(X, cells, nd);
        int gain = rs.prep_and_solve(X, cells, nd, cfg.merge_node_cap, rng.next());
        if (dbg) printf("merge nd=%d before=%d/%d gain=%d nodes=%llu\n", nd,
                        before, total_edges, gain, (unsigned long long)rs.nodes);
        if (gain > before) {
            g_merge_gains.fetch_add(1, std::memory_order_relaxed);
            g_polish_gains.fetch_add(1, std::memory_order_relaxed);
            report_completion(X);  // may be a new global best
        }
        g_plateau.add(X);  // drift variant or improvement; dedup absorbs no-ops
    }

    // one non-improving step passed: archive near-best drift, then either
    // keep counting (near-best lineages drift until the umbrella kick --
    // since_gain can exceed stagnation_kick only for them, which is what
    // makes umbrella_stagnation > stagnation_kick reachable at all; the old
    // unconditional reset silently disabled the umbrella) or resample.
    void bump_stagnation() {
        if (!local_only && cur_score >= 0 &&
            cur_score + 1 >= g_best_score.load(std::memory_order_relaxed))
            g_plateau.add(cur);
        if (++since_gain <= cfg.stagnation_kick) return;
        if (!local_only &&
            cur_score + 1 >= g_best_score.load(std::memory_order_relaxed))
            return;  // at/near best: keep drifting, harvest the plateau
        since_gain = 0;
        if (!local_only) cur_score = -1;  // resample a seed
    }

    // macro-LNS on stagnation: rotate the board, rebuild a whole horizontal
    // band of rows (y0..14) exactly under a mismatch budget, rotate back
    bool try_band(Rng& r) {
        int orient = (int)r.below(4);
        int y0 = cfg.band_y0_min + (int)r.below((u32)(13 - cfg.band_y0_min));
        Board rb = rotate_board(cur, orient);
        bool frozen[NC] = {};
        for (int s = 0; s < NC; s++)
            if (IS_CLUE_CELL[s]) frozen[rotate_cell(s, orient)] = true;
        int imp = bands.solve(rb, y0, frozen, cfg.band_node_cap, r.next());
        if (dbg) printf("band orient=%d y0=%d free=%d inc=%d best=%d nodes=%llu imp=%d\n",
                        orient, y0, bands.last_n_free, bands.last_incumbent_cost,
                        bands.last_best_cost, (unsigned long long)bands.last_nodes, imp);
        if (imp <= 0) return false;
        cur = rotate_board(rb, (4 - orient) & 3);
        cur_score = cur.score();
        g_polish_gains.fetch_add(1, std::memory_order_relaxed);
        since_gain = 0;
        if (!local_only) {
            if (cur_score > g_best_score.load(std::memory_order_relaxed))
                report_completion(cur);
            else if (cur_score + cfg.seed_slack >= g_best_score.load())
                g_seeds.add(cur);
        }
        return true;
    }

    int dbg = 0;  // 1: trace moves

    void step() {
        if (!local_only && !ensure_board()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return;
        }
        // plateau-merge (task #5 Part A): snapshot once when this polisher's
        // lineage first reaches (or ties) the global best, so the merge
        // driver sees independent same-score variants even outside umbrella
        // stagnation cycles. Latched on cur_score so it fires again only
        // after the score actually changes (ties at the same score count
        // once; a later different tying score re-arms it).
        if (!local_only && !cfg.drift_dir.empty() &&
            cur_score >= g_best_score.load(std::memory_order_relaxed) &&
            cur_score != drift_last_best_seen) {
            drift_snapshot(cur_score);
            drift_last_best_seen = cur_score;
        }
        g_polish_iters.fetch_add(1, std::memory_order_relaxed);
        // band rebuild fires every band_stagnation iters without gain
        // (modulo, not equality: near-best lineages no longer resample at
        // stagnation_kick, so the counter passes several multiples -- re-fire
        // on each). Masked/local polishers skip it (partial boards break the
        // pool/deficit invariants).
        if (!local_only && !mask && cfg.band_stagnation > 0 &&
            since_gain > 0 && since_gain % cfg.band_stagnation == 0)
            try_band(rng);
        // umbrella distance-kick: deep stagnation -> forced leap to a
        // different solution family (task #4 Part A). Fires once per
        // stagnation cycle, same discipline as band rebuild above; guarded
        // to !local_only since it targets the persistent main-pool polisher
        // threads, not the short-lived quadrant/seam workers used by
        // --mode decomp. Accepts unconditionally and resets since_gain
        // itself, so it always wins the race against the stagnation_kick
        // resample below (which only fires on since_gain, already 0 here).
        if (!local_only && cfg.umbrella && cfg.umbrella_stagnation > 0 &&
            since_gain == cfg.umbrella_stagnation) {
            // plateau-merge (task #5 Part A): capture this lineage's
            // sideways-drift variant right before the jump discards it, but
            // only if it's still near the global best (a merge target).
            if (!cfg.drift_dir.empty() &&
                cur_score >= g_best_score.load(std::memory_order_relaxed) - 1)
                drift_snapshot(cur_score);
            umbrella_jump();
            return;
        }
        // exact plateau merge: the 464 author's mass drift+merge, in-process
        if (!local_only && cfg.merge_pct > 0 &&
            (int)rng.below(100) < cfg.merge_pct && g_plateau.size() >= 2) {
            merge_move();
            bump_stagnation();
            return;
        }
        if (cfg.parity_pct > 0 && (int)rng.below(100) < cfg.parity_pct) {
            parity_move();
            bump_stagnation();
            return;
        }
        if (rng.below(100) < 50) {
            scatter_move();
            bump_stagnation();
            return;
        }
        int cells[40];
        int rcap = (int)rng.below(100) < cfg.region_big_pct ? cfg.region_max_big
                                                            : cfg.region_max;
        int n = pick_region(cells, rcap);
        if (n < 2) { if (!local_only) cur_score = -1; return; }

        // score of edges incident to region before; skip fault-free windows
        Board backup = cur;
        auto [before, total_edges] = incident_score_and_edges(cur, cells, n);
        if (before == total_edges) { bump_stagnation(); return; }  // nothing to gain here

        // LAHC exploration move (task #4 Part B): with probability
        // explore_pct, solve the region WITHOUT incumbent seeding (result may
        // be worse) and gate the resulting full-board score through a
        // per-thread LAHC history instead of the always-accept incumbent
        // path. Disabled entirely when lahc_len==0 (--lahc 0).
        bool do_explore = cfg.lahc_len > 0 &&
                          (int)rng.below(100) < cfg.explore_pct;
        if (do_explore) {
            lahc_ensure();
            int gain = rs.prep_and_solve(cur, cells, n, cfg.region_node_cap,
                                         rng.next(), /*seed_incumbent=*/false);
            int new_score = cur_score + (gain - before);
            int i = lahc_i;
            bool accept = new_score >= lahc_hist[i] || new_score >= cur_score;
            if (dbg) printf("lahc n=%d before=%d/%d gain=%d new=%d hist[%d]=%d "
                            "accept=%d\n", n, before, total_edges, gain,
                            new_score, i, lahc_hist[i], accept);
            if (accept) {
                cur_score = new_score;
                lahc_hist[i] = new_score;
                lahc_i = (i + 1) % (int)lahc_hist.size();
                if (gain > before) {
                    g_polish_gains.fetch_add(1, std::memory_order_relaxed);
                    since_gain = 0;
                    if ((g_polish_gains.load(std::memory_order_relaxed) & 1023) == 0 &&
                        cur.score() != cur_score) {
                        printf("[BUG] polisher incremental score drift: %d vs %d\n",
                               cur.score(), cur_score);
                        cur_score = cur.score();
                    }
                    if (!local_only) {
                        if (cur_score > g_best_score.load(std::memory_order_relaxed))
                            report_completion(cur);
                        else if (cur_score + cfg.seed_slack >= g_best_score.load())
                            g_seeds.add(cur);
                    }
                }
                // gain <= before (score flat or dropped) but LAHC accepted:
                // sideways/backward drift, already applied by prep_and_solve;
                // nothing further to report (score-gated pool paths untouched)
            } else {
                cur = backup;  // reject: exact restore of pre-move arrangement
            }
            bump_stagnation();
            return;
        }

        int gain = rs.prep_and_solve(cur, cells, n, cfg.region_node_cap, rng.next());
        if (dbg) printf("bnb n=%d before=%d/%d gain=%d nodes=%llu\n", n, before,
                        total_edges, gain, (unsigned long long)rs.nodes);
        if (gain > before || (gain == before && (int)rng.below(100) < cfg.tie_pct)) {
            if (gain > before) {
                cur_score += gain - before;
                g_polish_gains.fetch_add(1, std::memory_order_relaxed);
                since_gain = 0;
                if ((g_polish_gains.load(std::memory_order_relaxed) & 1023) == 0 &&
                    cur.score() != cur_score) {
                    printf("[BUG] polisher incremental score drift: %d vs %d\n",
                           cur.score(), cur_score);
                    cur_score = cur.score();
                }
                if (!local_only) {
                    if (cur_score > g_best_score.load(std::memory_order_relaxed))
                        report_completion(cur);
                    else if (cur_score + cfg.seed_slack >= g_best_score.load())
                        g_seeds.add(cur);
                }
            }
        } else {
            cur = backup;  // reject
        }
        bump_stagnation();
    }

    // (matched, total) edges incident to region (region-region pairs once)
    static std::pair<int, int> incident_score_and_edges(const Board& b,
                                                        const int* cells, int n) {
        bool inr[NC] = {};
        for (int i = 0; i < n; i++) inr[cells[i]] = true;
        int m = 0, te = 0;
        for (int i = 0; i < n; i++) {
            int s = cells[i], x = s % W, y = s / W;
            static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
            for (int dir = 0; dir < 4; dir++) {
                int nx = x + DX[dir], ny = y + DY[dir];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                int ns = ny * W + nx;
                if (inr[ns] && ns < s) continue;
                if (!inr[ns] && !b.filled[ns]) continue;  // no edge to empty cell
                te++;
                u8 a = b.q(s)[dir], c = b.q(ns)[(dir + 2) & 3];
                if (a == c && a != 0) m++;
            }
        }
        return {m, te};
    }
};

// ------------------------------------------------------------------ load board from file / URL edges
static bool board_conforms(const Board& b) {
    for (int s = 0; s < NC; s++) {
        if (IS_CLUE_CELL[s] &&
            (!b.filled[s] || b.piece[s] != CLUE_AT[s] || b.rot[s] != CLUE_ROT[s]))
            return false;
        if (b.filled[s] && CELL_TYPE[s] != 0 &&
            ring_rot_for_cell(s, b.piece[s]) != b.rot[s])
            return false;  // grey must face outward on the rim
    }
    return true;
}

static bool load_board_file(const std::string& path, Board& b) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    std::getline(f, line);  // header
    memset(&b, 0, sizeof b);
    for (int s = 0; s < NC; s++) {
        int p, r;
        char colon;
        if (!(f >> p >> colon >> r)) return false;
        b.piece[s] = (u8)(p - 1);
        b.rot[s] = (u8)r;
        b.filled[s] = true;
    }
    return true;
}

static bool load_board_edges(std::string edges, Board& b) {
    // accept a bare 1024-letter string, a full bucas URL, or a path to a
    // file containing either
    std::error_code ec;
    if (fs::exists(edges, ec) && fs::is_regular_file(edges, ec)) {
        std::ifstream f(edges);
        edges.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
    }
    size_t pos = edges.find("board_edges=");
    if (pos != std::string::npos) {
        pos += 12;
        size_t end = pos;
        while (end < edges.size() && edges[end] >= 'a' && edges[end] <= 'w') end++;
        edges = edges.substr(pos, end - pos);
    } else if (edges.size() != NC * 4) {
        // fall back: first maximal run of exactly 1024 edge letters
        for (size_t i = 0, j; i < edges.size(); i = j + 1) {
            for (j = i; j < edges.size() && edges[j] >= 'a' && edges[j] <= 'w'; j++) {}
            if (j - i == NC * 4) { edges = edges.substr(i, NC * 4); break; }
        }
    }
    if (edges.size() != NC * 4) return false;
    memset(&b, 0, sizeof b);
    bool usedp[256] = {};
    for (int s = 0; s < NC; s++) {
        u8 q[4];
        for (int i = 0; i < 4; i++) q[i] = (u8)(edges[s * 4 + i] - 'a');
        int found = -1, frot = 0;
        for (int p = 0; p < 256 && found < 0; p++) {
            if (usedp[p]) continue;
            for (int k = 0; k < 4; k++) {
                const u8* pq = QUAD[p][k];
                if (pq[0] == q[0] && pq[1] == q[1] && pq[2] == q[2] && pq[3] == q[3]) {
                    found = p; frot = k; break;
                }
            }
        }
        if (found < 0) return false;
        usedp[found] = true;
        b.piece[s] = (u8)found;
        b.rot[s] = (u8)frot;
        b.filled[s] = true;
    }
    return true;
}

// ------------------------------------------------------------------ Track B: overlapping-quadrant decomposition
// --mode decomp: build a full board (greedy, or --seed-edges), split it into
// four 9x9 quadrants overlapping in a 2-cell cross (cols/rows 7-8), polish
// each quadrant independently (masked LNS, restricted to the pieces initially
// inside it) for --minutes in parallel, then merge + reconcile the overlap.
static int seam_mismatches(const Board& b, const bool* seam) {
    int mm = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int s = y * W + x;
            if (!b.filled[s]) continue;
            if (x + 1 < W && b.filled[s + 1] && (seam[s] || seam[s + 1])) {
                u8 a = b.q(s)[1], c = b.q(s + 1)[3];
                if (!(a == c && a != 0)) mm++;
            }
            if (y + 1 < H && b.filled[s + W] && (seam[s] || seam[s + W])) {
                u8 a = b.q(s)[2], c = b.q(s + W)[0];
                if (!(a == c && a != 0)) mm++;
            }
        }
    return mm;
}

static int run_decomp() {
    auto t0 = std::chrono::steady_clock::now();
    Rng rng((u64)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    Board B0;
    if (g_best_score.load() >= 0) {   // from --seed-edges / resumed best
        std::lock_guard<std::mutex> lk(g_best_mu);
        B0 = g_best_board;
    } else {
        auto* h = new Hunter(rng.next());
        h->greedy_complete();
        B0 = h->b;
        delete h;
    }
    int b0_score = B0.score();
    {
        std::ofstream f(cfg.out_dir + "/b0_edges.txt");
        f << B0.edges_string() << "\n";
    }
    auto t1 = std::chrono::steady_clock::now();
    printf("[decomp] initial board score=%d/480 (edges saved to %s/b0_edges.txt)\n",
           b0_score, cfg.out_dir.c_str());

    struct QuadDef { int x0, y0, x1, y1; };
    static const QuadDef qd[4] = {{0, 0, 8, 8}, {7, 0, 15, 8},
                                  {0, 7, 8, 15}, {7, 7, 15, 15}};
    static bool qmask[4][NC];
    static Board qb[4];
    int qs_before[4], qs_after[4];
    for (int k = 0; k < 4; k++) {
        for (int s = 0; s < NC; s++) {
            int x = s % W, y = s / W;
            qmask[k][s] = x >= qd[k].x0 && x <= qd[k].x1 &&
                          y >= qd[k].y0 && y <= qd[k].y1;
        }
        qb[k] = B0;
        for (int s = 0; s < NC; s++)
            if (!qmask[k][s]) qb[k].filled[s] = false;
        qs_before[k] = qb[k].score();
    }
    double sub_secs = cfg.minutes > 0 ? cfg.minutes * 60 : 60;
    printf("[decomp] polishing 4 quadrants (9x9, 2-cell overlap) for %.0fs in parallel\n",
           sub_secs);
    {
        u64 sd[4];
        for (int k = 0; k < 4; k++) sd[k] = rng.next();
        std::vector<std::thread> ts;
        for (int k = 0; k < 4; k++)
            ts.emplace_back([&, k] {
                Polisher p(sd[k]);
                p.local_only = true;
                p.mask = qmask[k];
                p.mx0 = qd[k].x0; p.my0 = qd[k].y0;
                p.mx1 = qd[k].x1; p.my1 = qd[k].y1;
                p.cur = qb[k];
                p.cur_score = qs_before[k];
                auto end = std::chrono::steady_clock::now() +
                           std::chrono::duration<double>(sub_secs);
                while (std::chrono::steady_clock::now() < end) p.step();
                qb[k] = p.cur;
                qs_after[k] = p.cur.score();
            });
        for (auto& t : ts) t.join();
    }
    auto t2 = std::chrono::steady_clock::now();
    printf("[decomp] quadrant internal scores: ");
    for (int k = 0; k < 4; k++) printf("%d->%d ", qs_before[k], qs_after[k]);
    printf("\n");

    // ---- merge: unanimous cells first, then best disagreeing proposal with
    // an unused piece, then greedy patch of leftovers (type-consistent)
    Board M;
    memset(&M, 0, sizeof M);
    bool used[256] = {};
    int disagree = 0, dup_evict = 0;
    for (int s = 0; s < NC; s++) {
        int np = 0;
        u8 pp[4], pr[4];
        for (int k = 0; k < 4; k++)
            if (qmask[k][s]) { pp[np] = qb[k].piece[s]; pr[np] = qb[k].rot[s]; np++; }
        bool agree = true;
        for (int i = 1; i < np; i++)
            if (pp[i] != pp[0] || pr[i] != pr[0]) agree = false;
        if (!agree) { disagree++; continue; }
        if (used[pp[0]]) { dup_evict++; continue; }
        M.piece[s] = pp[0]; M.rot[s] = pr[0]; M.filled[s] = true;
        used[pp[0]] = true;
    }
    static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
    auto local_gain = [&](int s, int p, int r) {
        const u8* q = QUAD[p][r];
        int g = 0;
        for (int dir = 0; dir < 4; dir++) {
            int nx = s % W + DX[dir], ny = s / W + DY[dir];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
            int ns = ny * W + nx;
            if (M.filled[ns] && M.q(ns)[(dir + 2) & 3] == q[dir] && q[dir] != 0) g++;
        }
        return g;
    };
    for (int s = 0; s < NC; s++) {   // disagreeing / evicted cells: proposals
        if (M.filled[s]) continue;
        int bg = -1, bp = -1, br = 0;
        for (int k = 0; k < 4; k++) {
            if (!qmask[k][s]) continue;
            int p = qb[k].piece[s], r = qb[k].rot[s];
            if (used[p]) continue;
            int g = local_gain(s, p, r);
            if (g > bg) { bg = g; bp = p; br = r; }
        }
        if (bp >= 0) {
            M.piece[s] = (u8)bp; M.rot[s] = (u8)br; M.filled[s] = true;
            used[bp] = true;
        }
    }
    int patched = 0;
    for (int s = 0; s < NC; s++) {   // leftovers: any unused piece, best fit
        if (M.filled[s]) continue;
        int ct = CELL_TYPE[s], bg = -1, bp = -1, br = 0;
        for (int p = 0; p < 256; p++) {
            if (used[p] || PTYPE[p] != ct) continue;
            int k0 = 0, k1 = 3;
            if (ct != 0) {
                int k = ring_rot_for_cell(s, p);
                if (k < 0) continue;
                k0 = k1 = k;
            }
            for (int r = k0; r <= k1; r++) {
                int g = local_gain(s, p, r);
                if (g > bg) { bg = g; bp = p; br = r; }
            }
        }
        M.piece[s] = (u8)bp; M.rot[s] = (u8)br; M.filled[s] = true;
        used[bp] = true;
        patched++;
    }
    static bool seam[NC];
    for (int s = 0; s < NC; s++) {
        int x = s % W, y = s / W;
        seam[s] = (x >= 6 && x <= 9) || (y >= 6 && y <= 9);
    }
    int mscore = M.score(), mseam = seam_mismatches(M, seam);
    printf("[decomp] merged: score=%d/480 overlap_disagreements=%d dup_evicted=%d "
           "greedy_patched=%d seam_mismatches=%d\n",
           mscore, disagree, dup_evict, patched, mseam);

    // ---- reconciliation: seam-restricted LNS (cells within 1 of the overlap
    // cross), 4 independent copies, best wins
    Board best_b = M;
    int best_sc = mscore;
    {
        std::mutex bmu;
        u64 sd[4];
        for (int k = 0; k < 4; k++) sd[k] = rng.next();
        std::vector<std::thread> ts;
        for (int k = 0; k < 4; k++)
            ts.emplace_back([&, k] {
                Polisher p(sd[k]);
                p.local_only = true;
                p.mask = seam;
                p.cur = M;
                p.cur_score = mscore;
                auto end = std::chrono::steady_clock::now() +
                           std::chrono::duration<double>(cfg.rec_seconds);
                while (std::chrono::steady_clock::now() < end) p.step();
                int sc = p.cur.score();
                std::lock_guard<std::mutex> lk(bmu);
                if (sc > best_sc) { best_sc = sc; best_b = p.cur; }
            });
        for (auto& t : ts) t.join();
    }
    auto t3 = std::chrono::steady_clock::now();
    int fseam = seam_mismatches(best_b, seam);

    bool ok = board_conforms(best_b);
    {
        bool u2[256] = {};
        for (int s = 0; s < NC; s++) {
            if (!best_b.filled[s] || u2[best_b.piece[s]]) { ok = false; break; }
            u2[best_b.piece[s]] = true;
        }
    }
    auto secs = [](auto a, auto b) {
        return std::chrono::duration<double>(b - a).count();
    };
    printf("[decomp] reconciled: score=%d/480 seam_mismatches=%d valid=%s\n",
           best_sc, fseam, ok ? "yes" : "NO");
    printf("[decomp] time: init %.1fs, quadrants %.1fs, reconcile %.1fs\n",
           secs(t0, t1), secs(t1, t2), secs(t2, t3));
    if (ok) report_completion(best_b);
    printf("final best %d/480\n", g_best_score.load());
    return 0;
}

// ------------------------------------------------------------------ good-group generator (--mode make-groups)
// LV's tileability measure adapted to our architecture: our ring is solved
// separately and the interior DFS never places border pieces, so a group's
// tileability = number of ways 6 distinct group pieces fill a 3x2 rectangle
// of interior cells with all 7 internal edges matched (outer sides free).
// Groups improve by random piece swaps kept when the count does not drop
// (LV: "if the tileability improves, the pieces are really switched").
struct GroupGen {
    std::vector<u32> ALL, LBY[23], UBY[23], LU[23][23];
    bool ing[256];     // piece in current group
    bool gused[256];   // piece placed in current 3x2 enumeration
    int cnt[23][23];   // unused group candidates by [left][up] (closed-form last cell)

    void build_master() {
        for (int p = 0; p < 256; p++) {
            if (PTYPE[p] != 0) continue;
            for (int k = 0; k < 4; k++) {
                u32 v = pack(p, k, QUAD[p][k]);
                ALL.push_back(v);
                LBY[CW(v)].push_back(v);
                UBY[CN(v)].push_back(v);
                LU[CW(v)][CN(v)].push_back(v);
            }
        }
    }
    inline void toggle(int p, int dv) {
        for (int k = 0; k < 4; k++) {
            const u8* q = QUAD[p][k];
            cnt[q[3]][q[0]] += dv;   // [left][up]
        }
    }
    inline void pl(int p) { gused[p] = true; toggle(p, -1); }
    inline void un(int p) { gused[p] = false; toggle(p, +1); }

    // cells (row,col): (0,0)(0,1)(1,0)(1,1)(2,0) enumerated, (2,1) counted
    // in closed form from cnt (which tracks used pieces via pl/un toggles)
    u64 eval(const std::vector<int>& group) {
        memset(ing, 0, sizeof ing);
        memset(gused, 0, sizeof gused);
        memset(cnt, 0, sizeof cnt);
        for (int p : group) { ing[p] = true; toggle(p, +1); }
        u64 total = 0;
        for (u32 v0 : ALL) {
            if (!ing[CP(v0)]) continue;
            pl(CP(v0));
            for (u32 v1 : LBY[CE(v0)]) {                  // (0,1) left=E(v0)
                if (!ing[CP(v1)] || gused[CP(v1)]) continue;
                pl(CP(v1));
                for (u32 v2 : UBY[CS(v0)]) {              // (1,0) up=S(v0)
                    if (!ing[CP(v2)] || gused[CP(v2)]) continue;
                    pl(CP(v2));
                    for (u32 v3 : LU[CE(v2)][CS(v1)]) {   // (1,1)
                        if (!ing[CP(v3)] || gused[CP(v3)]) continue;
                        pl(CP(v3));
                        for (u32 v4 : UBY[CS(v2)]) {      // (2,0) up=S(v2)
                            if (!ing[CP(v4)] || gused[CP(v4)]) continue;
                            pl(CP(v4));
                            total += (u64)cnt[CE(v4)][CS(v3)];  // (2,1)
                            un(CP(v4));
                        }
                        un(CP(v3));
                    }
                    un(CP(v2));
                }
                un(CP(v1));
            }
            un(CP(v0));
        }
        return total;
    }
};

static int run_make_groups() {
    std::vector<int> interior;
    for (int p = 0; p < 256; p++)
        if (PTYPE[p] == 0) interior.push_back(p);
    int gsz = std::max(8, std::min((int)interior.size() - 8, cfg.group_size));
    printf("make-groups: n=%d size=%d iters=%d threads=%d -> %s\n",
           cfg.groups_n, gsz, cfg.groups_iters, cfg.threads,
           cfg.groups_out.c_str());
    fflush(stdout);
    std::vector<std::pair<u64, std::vector<int>>> results(cfg.groups_n);
    std::atomic<int> next_g{0};
    std::mutex mu;
    Rng seeder((u64)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::vector<u64> seeds(cfg.groups_n);
    for (auto& s : seeds) s = seeder.next();
    auto worker = [&]() {
        GroupGen gg;
        gg.build_master();
        for (;;) {
            int gi = next_g.fetch_add(1);
            if (gi >= cfg.groups_n) return;
            Rng rng(seeds[gi]);
            std::vector<int> pool = interior;
            std::shuffle(pool.begin(), pool.end(), std::mt19937_64(rng.next()));
            std::vector<int> group(pool.begin(), pool.begin() + gsz);
            std::vector<int> outside(pool.begin() + gsz, pool.end());
            u64 cur = gg.eval(group), start = cur, bestv = cur;
            std::vector<int> bestg = group;
            for (int it = 0; it < cfg.groups_iters; it++) {
                int i = (int)rng.below((u32)group.size());
                int j = (int)rng.below((u32)outside.size());
                std::swap(group[i], outside[j]);
                u64 nv = gg.eval(group);
                if (nv >= cur) {   // >= : free sideways drift on plateaus
                    cur = nv;
                    if (nv > bestv) { bestv = nv; bestg = group; }
                } else std::swap(group[i], outside[j]);
            }
            results[gi] = {bestv, std::move(bestg)};
            std::lock_guard<std::mutex> lk(mu);
            printf("[group %2d] tileability %llu -> %llu\n", gi,
                   (unsigned long long)start, (unsigned long long)bestv);
            fflush(stdout);
        }
    };
    std::vector<std::thread> ts;
    for (int i = 0; i < cfg.threads; i++) ts.emplace_back(worker);
    for (auto& t : ts) t.join();
    fs::path outp(cfg.groups_out);
    if (outp.has_parent_path()) fs::create_directories(outp.parent_path());
    std::ofstream f(cfg.groups_out, std::ios::trunc);
    f << "# good groups (tileability = 3x2 interior fill count), piece ids 1-based\n";
    f << "# n=" << cfg.groups_n << " size=" << gsz
      << " iters=" << cfg.groups_iters << "\n";
    for (auto& r : results) {
        std::vector<int> g = r.second;
        std::sort(g.begin(), g.end());
        f << r.first;
        for (int p : g) f << ' ' << p + 1;
        f << "\n";
    }
    printf("wrote %d groups to %s\n", cfg.groups_n, cfg.groups_out.c_str());
    return 0;
}

// ------------------------------------------------------------------ threads & main
static void hunter_thread(u64 seed) {
    Hunter h(seed);
    while (!g_stop.load()) h.run_restart();
}

static void polisher_thread(u64 seed, int id) {
    Polisher p(seed);
    p.thread_id = id;
    while (!g_stop.load()) p.step();
}

static void mix_thread(u64 seed, int id) {
    Hunter h(seed);
    Polisher p(seed ^ 0xabcdef123456ULL);
    p.thread_id = id;
    while (!g_stop.load()) {
        if (g_fresh.size() < 8) { h.greedy_complete(); continue; }
        if (p.rng.uniform() < cfg.polish_frac) {
            for (int i = 0; i < 400 && !g_stop.load(); i++) p.step();
        } else {
            // a hunt turn is a node budget, not one restart: under a slip
            // schedule most restarts die within microseconds at tiny depth,
            // and a single-restart turn starves the hunters against the
            // multi-second polish blocks. One full-cap restart (the old
            // behavior when restarts always exhausted node_cap) == one turn.
            u64 spent = 0;
            do { h.run_restart(); spent += h.nodes; }
            while (spent < cfg.node_cap && !g_stop.load());
        }
    }
}

static void status_thread() {
    auto t0 = std::chrono::steady_clock::now();
    u64 last_nodes = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        auto el = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - t0).count();
        u64 n = g_nodes.load();
        u64 rs = g_restarts.load();
        printf("[%6llds] best=%d/480 restarts=%llu avgdepth=%llu maxdepth=%d "
               "completions=%llu hyb=%llu ab=%llu nodes=%.2fB (%.1fM/s) polish=%llu(+%llu) "
               "seeds=%zu fresh=%zu plat=%zu umbrella=%llu tieproof=%llu "
               "parity=%llu(+%llu) merge=%llu(+%llu)\n",
               (long long)el, g_best_score.load(), (unsigned long long)rs,
               (unsigned long long)(rs ? g_depth_sum.load() / rs : 0),
               g_depth_max.load(),
               (unsigned long long)g_completions.load(),
               (unsigned long long)g_hybrid.load(),
               (unsigned long long)g_aborts.load(), n / 1e9,
               (n - last_nodes) / 15.0e6, (unsigned long long)g_polish_iters.load(),
               (unsigned long long)g_polish_gains.load(), g_seeds.size(), g_fresh.size(),
               g_plateau.size(),
               (unsigned long long)g_umbrella_jumps.load(),
               (unsigned long long)g_region_tieproof.load(),
               (unsigned long long)g_parity_iters.load(),
               (unsigned long long)g_parity_gains.load(),
               (unsigned long long)g_merge_iters.load(),
               (unsigned long long)g_merge_gains.load());
        last_nodes = n;
        fflush(stdout);
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--clues") cfg.clues = std::stoi(next());
        else if (a == "--threads") cfg.threads = std::stoi(next());
        else if (a == "--mode") cfg.mode = next();
        else if (a == "--out") cfg.out_dir = next();
        else if (a == "--node-cap") cfg.node_cap = std::stoull(next());
        else if (a == "--budget-cap") cfg.budget_cap = std::stoi(next());
        else if (a == "--pre-budget") cfg.pre_budget = std::stoi(next());
        else if (a == "--seed-slack") cfg.seed_slack = std::stoi(next());
        else if (a == "--fr-min") cfg.fr_min = std::stoi(next());
        else if (a == "--fr-max") cfg.fr_max = std::stoi(next());
        else if (a == "--minutes") cfg.minutes = std::stod(next());
        else if (a == "--region-max") cfg.region_max = std::max(8, std::min(40, std::stoi(next())));
        else if (a == "--region-nodes") cfg.region_node_cap = std::stoull(next());
        else if (a == "--band-stagnation") cfg.band_stagnation = std::stoi(next());
        else if (a == "--stagnation-kick") cfg.stagnation_kick = std::stoi(next());
        else if (a == "--band-nodes") cfg.band_node_cap = std::stoull(next());
        else if (a == "--selftest") cfg.selftest = true;
        else if (a == "--polish-frac") cfg.polish_frac = std::stod(next());
        else if (a == "--seed-edges") cfg.seed_url = next();
        else if (a == "--order") cfg.order = next();
        else if (a == "--model-file") cfg.model_file = next();
        else if (a == "--zone-file") cfg.zone_file = next();
        else if (a == "--log-placements") cfg.log_placements = next();
        else if (a == "--rec-seconds") cfg.rec_seconds = std::stod(next());
        else if (a == "--umbrella") cfg.umbrella = std::stoi(next()) != 0;
        else if (a == "--umbrella-dist") cfg.umbrella_dist = std::stoi(next());
        else if (a == "--umbrella-stagnation") cfg.umbrella_stagnation = std::stoi(next());
        else if (a == "--explore-pct") cfg.explore_pct = std::stoi(next());
        else if (a == "--lahc") cfg.lahc_len = std::stoi(next());
        else if (a == "--drift-dir") cfg.drift_dir = next();
        else if (a == "--cost2") cfg.cost2 = std::stoi(next());
        else if (a == "--parity-pct") cfg.parity_pct = std::stoi(next());
        else if (a == "--band-y0-min") cfg.band_y0_min = std::max(1, std::min(12, std::stoi(next())));
        else if (a == "--hybrid") cfg.hybrid = std::stoi(next()) != 0;
        else if (a == "--tie-pct") cfg.tie_pct = std::stoi(next());
        else if (a == "--merge-pct") cfg.merge_pct = std::stoi(next());
        else if (a == "--merge-nodes") cfg.merge_node_cap = std::stoull(next());
        else if (a == "--plateau-cap") cfg.plateau_cap = std::stoi(next());
        else if (a == "--region-big-pct") cfg.region_big_pct = std::stoi(next());
        else if (a == "--region-max-big") cfg.region_max_big = std::max(8, std::min(40, std::stoi(next())));
        else if (a == "--groups-file") cfg.groups_file = next();
        else if (a == "--group-order") cfg.group_order = std::stoi(next());
        else if (a == "--group-maxgood") cfg.group_maxgood = std::stoi(next());
        else if (a == "--group-n-precious") cfg.group_n_prec = std::stoi(next());
        else if (a == "--group-n-useless") cfg.group_n_useless = std::stoi(next());
        else if (a == "--group-cks") {
            if (sscanf(next().c_str(), "%d,%d,%d,%d", &cfg.group_ck[0],
                       &cfg.group_ck[1], &cfg.group_ck[2], &cfg.group_ck[3]) != 4) {
                printf("ERROR: --group-cks wants A,B,C,D\n");
                return 1;
            }
        }
        else if (a == "--slip-target") cfg.slip_target = std::stoi(next());
        else if (a == "--slip-span") cfg.slip_span = std::stod(next());
        else if (a == "--abort-points") cfg.abort_points = next();
        else if (a == "--tail-cols") cfg.tail_cols = std::max(0, std::min(10, std::stoi(next())));
        else if (a == "--group-size") cfg.group_size = std::stoi(next());
        else if (a == "--groups-n") cfg.groups_n = std::stoi(next());
        else if (a == "--groups-iters") cfg.groups_iters = std::stoi(next());
        else if (a == "--groups-out") cfg.groups_out = next();
        else { printf("unknown arg %s\n", a.c_str()); return 1; }
    }
    if (cfg.threads <= 0) {
        int hc = (int)std::thread::hardware_concurrency();
        cfg.threads = std::max(1, hc - 2);
    }
    init_static(cfg.clues);
    init_ring_cells();
    g_plateau.cap = (size_t)std::max(2, cfg.plateau_cap);
    fs::create_directories(cfg.out_dir);
    if (!cfg.drift_dir.empty()) fs::create_directories(cfg.drift_dir);
    if (!cfg.model_file.empty() && !load_model(cfg.model_file)) {
        printf("ERROR: cannot load model file %s\n", cfg.model_file.c_str());
        return 1;
    }
    if (!cfg.zone_file.empty() && !load_zone(cfg.zone_file)) {
        printf("ERROR: cannot load zone file %s\n", cfg.zone_file.c_str());
        return 1;
    }
    if ((cfg.order == "model" || cfg.order == "model+zone") && !g_model.loaded) {
        printf("ERROR: --order %s needs --model-file\n", cfg.order.c_str());
        return 1;
    }
    if ((cfg.order == "zone" || cfg.order == "model+zone") && !g_zone_loaded) {
        printf("ERROR: --order %s needs --zone-file\n", cfg.order.c_str());
        return 1;
    }
    if (!cfg.log_placements.empty()) {
        g_log.open(cfg.log_placements, std::ios::app);
        if (!g_log) {
            printf("ERROR: cannot open %s\n", cfg.log_placements.c_str());
            return 1;
        }
    }
    if (!cfg.groups_file.empty() && !load_groups(cfg.groups_file)) {
        printf("ERROR: cannot load groups file %s\n", cfg.groups_file.c_str());
        return 1;
    }
    if (!cfg.abort_points.empty()) {
        const char* s = cfg.abort_points.c_str();
        while (*s) {
            char* e;
            u64 nn = strtoull(s, &e, 10);
            if (*e != ':') { printf("ERROR: --abort-points wants nodes:mindepth,...\n"); return 1; }
            long dmin = strtol(e + 1, &e, 10);
            g_abort_ms.push_back({nn, (int)dmin});
            if (*e == ',') s = e + 1;
            else if (*e == 0) s = e;
            else { printf("ERROR: --abort-points wants nodes:mindepth,...\n"); return 1; }
        }
        std::sort(g_abort_ms.begin(), g_abort_ms.end());
    }

    printf("eternity2 solver | clues=%d threads=%d mode=%s order=%s out=%s\n",
           cfg.clues, cfg.threads, cfg.mode.c_str(), cfg.order.c_str(),
           cfg.out_dir.c_str());
    if (g_groups_loaded)
        printf("groups: %zu from %s (maxgood=%d prec/useless=%d/%d cks=%d,%d,%d,%d)\n",
               g_groups.size(), cfg.groups_file.c_str(), cfg.group_maxgood,
               cfg.group_n_prec, cfg.group_n_useless, cfg.group_ck[0],
               cfg.group_ck[1], cfg.group_ck[2], cfg.group_ck[3]);
    if (cfg.slip_target > 0)
        printf("slip: target %d (T=%d) span=%.2f\n", cfg.slip_target,
               MAX_SCORE - cfg.slip_target, cfg.slip_span);
    if (!g_abort_ms.empty())
        printf("abort points: %zu milestones\n", g_abort_ms.size());
    if (cfg.tail_cols > 0)
        printf("tail-cols: last %d interior rows column-wise\n", cfg.tail_cols);

    if (cfg.mode == "make-groups") return run_make_groups();

    // resume from best file / seed edges
    Board b;
    if (load_board_file(best_path(), b)) {
        if (board_conforms(b)) {
            printf("resumed best from %s: %d/480\n", best_path().c_str(), b.score());
            report_completion(b);
        } else printf("WARN: %s does not honor clues/rim; ignored\n", best_path().c_str());
    }
    if (!cfg.seed_url.empty()) {
        if (load_board_edges(cfg.seed_url, b) && board_conforms(b)) {
            printf("seed board loaded: %d/480\n", b.score());
            report_completion(b);
        } else printf("WARN: --seed-edges rejected (unparseable, wrong pieces, or clue mismatch)\n");
    }

    if (cfg.selftest) {
        Board B;
        if (g_best_score.load() >= 0) {
            std::lock_guard<std::mutex> lk(g_best_mu);
            B = g_best_board;
        } else {
            auto* h = new Hunter(12345);
            h->greedy_complete();
            B = h->b;
            delete h;
        }
        int s0 = B.score();
        bool ok = board_conforms(B);
        if (!ok) printf("selftest: base board does not conform\n");
        for (int k = 0; k < 4; k++) {
            Board r = rotate_board(B, k);
            if (r.score() != s0) {
                printf("selftest FAIL: score not invariant at k=%d (%d vs %d)\n",
                       k, r.score(), s0);
                ok = false;
            }
            for (int s = 0; s < NC; s++)
                if (IS_CLUE_CELL[s]) {
                    int ds = rotate_cell(s, k);
                    if (!r.filled[ds] || r.piece[ds] != B.piece[s] ||
                        r.rot[ds] != ((B.rot[s] + k) & 3)) {
                        printf("selftest FAIL: clue cell %d mismapped at k=%d\n", s, k);
                        ok = false;
                    }
                }
            Board back = rotate_board(r, (4 - k) & 3);
            for (int s = 0; s < NC; s++) {
                if (back.filled[s] != B.filled[s] ||
                    (B.filled[s] && (back.piece[s] != B.piece[s] ||
                                     back.rot[s] != B.rot[s]))) {
                    printf("selftest FAIL: rotate k=%d round trip differs at cell %d\n", k, s);
                    ok = false;
                    break;
                }
            }
        }
        printf("selftest %s (base score %d/480)\n", ok ? "PASS" : "FAIL", s0);
        return ok ? 0 : 1;
    }

    if (cfg.mode == "band") {
        if (g_best_score.load() < 0) {
            printf("ERROR: --mode band requires a seeded board (--seed-edges or resume)\n");
            return 1;
        }
        std::thread wd;
        if (cfg.minutes > 0)
            wd = std::thread([] {
                auto end = std::chrono::steady_clock::now() +
                           std::chrono::seconds((long long)(cfg.minutes * 60));
                while (!g_stop.load() && std::chrono::steady_clock::now() < end)
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                g_stop.store(true);
            });
        Board B;
        {
            std::lock_guard<std::mutex> lk(g_best_mu);
            B = g_best_board;
        }
        int score = B.score();
        printf("[band] start score=%d/480 node_cap=%llu\n", score,
               (unsigned long long)cfg.band_node_cap);
        Rng rng((u64)std::chrono::high_resolution_clock::now().time_since_epoch().count());
        auto* bs = new BandSolver();
        int rc = 0;
        while (!g_stop.load()) {
            for (int orient = 0; orient < 4 && !g_stop.load() && !rc; orient++)
                for (int y0 = cfg.band_y0_min; y0 <= 12 && !g_stop.load() && !rc; y0++) {
                    Board rb = rotate_board(B, orient);
                    bool frozen[NC] = {};
                    for (int s = 0; s < NC; s++)
                        if (IS_CLUE_CELL[s]) frozen[rotate_cell(s, orient)] = true;
                    int imp = bs->solve(rb, y0, frozen, cfg.band_node_cap, rng.next());
                    printf("[band] orient=%d y0=%d free=%d inc_cost=%d best_cost=%d "
                           "nodes=%llu imp=%d\n",
                           orient, y0, bs->last_n_free, bs->last_incumbent_cost,
                           bs->last_best_cost, (unsigned long long)bs->last_nodes, imp);
                    fflush(stdout);
                    if (imp > 0) {
                        Board nb = rotate_board(rb, (4 - orient) & 3);
                        if (!board_conforms(nb)) {
                            printf("[band] ERROR: improved board violates clues/rim\n");
                            rc = 1;
                            break;
                        }
                        int nsc = nb.score();
                        if (nsc != score + imp) {
                            printf("[band] ERROR: score %d + imp %d != recount %d\n",
                                   score, imp, nsc);
                            rc = 1;
                            break;
                        }
                        B = nb;
                        score = nsc;
                        report_completion(B);
                    }
                }
            if (rc) break;
        }
        delete bs;
        g_stop.store(true);
        if (wd.joinable()) wd.join();
        printf("final best %d/480\n", g_best_score.load());
        return rc;
    }

    if (cfg.mode == "decomp") return run_decomp();

    if (cfg.mode == "test-polish") {
        if (g_best_score.load() < 0) { printf("no board to test\n"); return 1; }
        Polisher p(12345);
        p.dbg = 1;
        {
            std::lock_guard<std::mutex> lk(g_best_mu);
            p.cur = g_best_board;
        }
        p.cur_score = p.cur.score();
        printf("start score %d\n", p.cur_score);
        for (int i = 0; i < 60; i++) p.step();
        printf("end score %d (recount %d)\n", p.cur_score, p.cur.score());
        return 0;
    }

    std::vector<std::thread> ts;
    Rng seeder((u64)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    for (int i = 0; i < cfg.threads; i++) {
        u64 sd = seeder.next();
        if (cfg.mode == "hunt") ts.emplace_back(hunter_thread, sd);
        else if (cfg.mode == "polish") ts.emplace_back(polisher_thread, sd, i);
        else ts.emplace_back(mix_thread, sd, i);
    }
    if (cfg.minutes > 0)
        ts.emplace_back([] {
            auto end = std::chrono::steady_clock::now() + std::chrono::seconds((long long)(cfg.minutes * 60));
            while (!g_stop.load() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            g_stop.store(true);
        });
    status_thread();  // runs until g_stop
    for (auto& t : ts) t.join();
    u64 frs = g_restarts.load();
    printf("final best %d/480 restarts=%llu avgdepth=%llu maxdepth=%d\n",
           g_best_score.load(), (unsigned long long)frs,
           (unsigned long long)(frs ? g_depth_sum.load() / frs : 0),
           g_depth_max.load());
    return 0;
}

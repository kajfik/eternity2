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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
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
    double minutes = 0;         // stop after this many minutes (0 = run forever)
    std::string seed_url;       // optional: start polishing from this board
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

static void init_static(int nclues) {
    for (int p = 0; p < 256; p++) {
        int g = 0;
        for (int i = 0; i < 4; i++) g += e2::PIECE_DEFS[p][i] == 0;
        PTYPE[p] = g == 0 ? 0 : (g == 1 ? 1 : 2);
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < 4; i++)
                QUAD[p][k][i] = e2::PIECE_DEFS[p][(i - k) & 3];
    }
    for (int s = 0; s < NC; s++) {
        int x = s % W, y = s / W;
        bool bx = x == 0 || x == W - 1, by = y == 0 || y == H - 1;
        CELL_TYPE[s] = bx && by ? 2 : (bx || by ? 1 : 0);
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

// ------------------------------------------------------------------ global best + seed pool
struct SeedPool {
    std::mutex mu;
    std::vector<Board> pool;
    size_t cap = 160;
    u64 tick = 0x2545F4914F6CDD1DULL;
    void add(const Board& b) {
        std::lock_guard<std::mutex> lk(mu);
        tick = tick * 6364136223846793005ULL + 1442695040888963407ULL;
        if (pool.size() < cap) pool.push_back(b);
        else pool[(tick >> 33) % cap] = b;
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
static std::atomic<u64> g_nodes{0}, g_restarts{0}, g_completions{0},
    g_polish_iters{0}, g_polish_gains{0}, g_depth_sum{0},
    g_scatter_iters{0}, g_scatter_gains{0};
static std::atomic<bool> g_stop{false};

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
    std::vector<u32> exact[23][23];  // [w][n]
    std::vector<u32> by_w[23];       // north differs
    std::vector<u32> by_n[23];       // west differs
    void build(Rng& rng, const bool* piece_excluded) {
        for (auto& row : exact) for (auto& v : row) v.clear();
        for (auto& v : by_w) v.clear();
        for (auto& v : by_n) v.clear();
        std::vector<u32> all;
        for (int p = 0; p < 256; p++) {
            if (PTYPE[p] != 0 || piece_excluded[p]) continue;
            for (int k = 0; k < 4; k++) all.push_back(pack(p, k, QUAD[p][k]));
        }
        std::shuffle(all.begin(), all.end(), std::mt19937_64(rng.next()));
        for (u32 v : all) {
            exact[CW(v)][CN(v)].push_back(v);
            by_w[CW(v)].push_back(v);
            by_n[CN(v)].push_back(v);
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

    explicit Hunter(u64 seed) : rng(seed) {}

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
        bk.build(rng, excl);
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
        for (int y = 1; y < H - 1; y++)
            for (int x = 1; x < W - 1; x++)
                if (!IS_CLUE_CELL[y * W + x]) scan[n_scan++] = y * W + x;
        free_row = cfg.fr_min + (int)rng.below((u32)(cfg.fr_max - cfg.fr_min + 1));
        int cur_best = g_best_score.load(std::memory_order_relaxed);
        int best_m = cur_best < 0 ? 1 << 20 : MAX_SCORE - cur_best;
        budget = std::min(cfg.budget_cap, best_m + cfg.seed_slack);
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
        if (d > max_depth) max_depth = d;
        int s = scan[d];
        int wcol = b.q(s - 1)[1];   // west neighbor's east side
        int ncol = b.q(s - W)[2];   // north neighbor's south side
        bool pre = (s / W) < free_row;

        auto try_list = [&](const std::vector<u32>& list, bool skip_exact_n,
                            bool skip_exact_w) {
            for (u32 v : list) {
                if (used[CP(v)]) continue;
                if (skip_exact_n && (int)CN(v) == ncol) continue;
                if (skip_exact_w && (int)CW(v) == wcol) continue;
                nodes++;
                int c = place(s, CP(v), CR(v));
                if (cost + c + deficit > budget ||
                    (pre && cost_pre + c > cfg.pre_budget)) {
                    unplace(s);
                    continue;
                }
                dfs(d + 1, cost + c, cost_pre + (pre ? c : 0));
                unplace(s);
                if (nodes > node_cap || g_stop.load(std::memory_order_relaxed)) return;
            }
        };

        const auto& exact = bk.exact[wcol][ncol];
        try_list(exact, false, false);
        if (nodes > node_cap || g_stop.load(std::memory_order_relaxed)) return;
        // mismatch placements: in the prefix only as a fallback when no exact
        // candidate exists (keeps prefix branching tiny); free in the tail
        bool may_mismatch = pre ? (cost_pre < cfg.pre_budget && exact.empty())
                                : true;
        if (may_mismatch && budget - cost > deficit) {
            try_list(bk.by_w[wcol], true, false);   // north mismatched
            if (nodes > node_cap) return;
            try_list(bk.by_n[ncol], false, true);   // west mismatched
        }
    }

    void run_restart() {
        if (!setup_restart()) return;
        max_depth = 0;
        dfs(0, 0, 0);
        g_nodes.fetch_add(nodes, std::memory_order_relaxed);
        g_restarts.fetch_add(1, std::memory_order_relaxed);
        g_depth_sum.fetch_add((u64)max_depth, std::memory_order_relaxed);
    }

    // fast full-board construction: min-mismatch piece at each cell, random
    // tie-break; always completes -> seeds the polisher pool
    void greedy_complete() {
        if (!setup_restart()) return;
        int total = 0;
        for (int d = 0; d < n_scan; d++) {
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
        g_completions.fetch_add(1, std::memory_order_relaxed);
        report_completion(b);
        g_restarts.fetch_add(1, std::memory_order_relaxed);
    }
};

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
    u64 nodes, cap;
    Rng rrng{1};                   // tie-break randomization (sideways drift)

    // count edge match: returns 1 if colors equal and non-grey
    static inline int em(u8 a, u8 c) { return a == c && a != 0; }

    int prep_and_solve(Board& board, const int* cs, int n, u64 node_cap, u64 seed) {
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
        // decided-edge counts for bound
        for (int i = 0; i < n; i++) {
            int s = cells[i], x = s % W, y = s / W;
            int dcount = 0;
            static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
            for (int dir = 0; dir < 4; dir++) {
                int nx = x + DX[dir], ny = y + DY[dir];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                int ns = ny * W + nx;
                if (board.filled[ns]) dcount++;       // fixed neighbor
                else {
                    // earlier region cell?
                    for (int j = 0; j < i; j++)
                        if (cells[j] == ns) { dcount++; break; }
                }
            }
            decided[i] = dcount;
        }
        suffix_max[n] = 0;
        for (int i = n - 1; i >= 0; i--) suffix_max[i] = suffix_max[i + 1] + decided[i];
        rrng.s = seed;
        // seed the search with the incumbent (original arrangement): capped
        // searches can then never lose ground, and the strong initial lower
        // bound prunes most of the tree from the start
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
        best_gain = inc_gain - 1;  // ties may still replace the incumbent (drift)
        nodes = 0;
        cap = node_cap;
        rec(0, 0);
        // apply best
        for (int i = 0; i < n; i++) {
            board.piece[cells[i]] = best_piece[i];
            board.rot[cells[i]] = best_rot[i];
            board.filled[cells[i]] = true;
        }
        return std::max(best_gain, inc_gain);
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
        if (gain + suffix_max[i] <= best_gain) return;   // bound
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
        for (int ci = 0; ci < nc; ci++) {
            // candidates sorted by g desc: once one is prunable, all are
            if (gain + cand[ci].g + suffix_max[i + 1] <= best_gain) break;
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
    int since_gain = 0;

    explicit Polisher(u64 seed) : rng(seed) {}

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
        return true;
    }

    // collect cells touching a mismatched edge (excluding clue cells)
    int fault_cells(int* out) {
        bool mark[NC] = {};
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int s = y * W + x;
                if (x + 1 < W) {
                    u8 a = cur.q(s)[1], b2 = cur.q(s + 1)[3];
                    if (!(a == b2 && a != 0)) mark[s] = mark[s + 1] = true;
                }
                if (y + 1 < H) {
                    u8 a = cur.q(s)[2], b2 = cur.q(s + W)[0];
                    if (!(a == b2 && a != 0)) mark[s] = mark[s + W] = true;
                }
            }
        int n = 0;
        for (int s = 0; s < NC; s++)
            if (mark[s] && !IS_CLUE_CELL[s]) out[n++] = s;
        return n;
    }

    // pick region cells for B&B re-solve: rectangle or fault blob
    int pick_region(int* out) {
        int roll = (int)rng.below(100);
        if (roll < 50) {  // rectangle window
            int w = 2 + (int)rng.below(4), h = 2 + (int)rng.below(4);
            while (w * h > cfg.region_max) (rng.below(2) ? w : h)--;
            int x0 = (int)rng.below(W - w + 1), y0 = (int)rng.below(H - h + 1);
            int n = 0;
            for (int y = y0; y < y0 + h; y++)
                for (int x = x0; x < x0 + w; x++) {
                    int s = y * W + x;
                    if (!IS_CLUE_CELL[s]) out[n++] = s;
                }
            return n;
        }
        // blob grown from a random conflicted edge
        int confl[480][2], nc = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int s = y * W + x;
                if (x + 1 < W) {
                    u8 a = cur.q(s)[1], b2 = cur.q(s + 1)[3];
                    if (!(a == b2 && a != 0)) { confl[nc][0] = s; confl[nc][1] = s + 1; nc++; }
                }
                if (y + 1 < H) {
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
            if (!IS_CLUE_CELL[s] && !inr[s]) { inr[s] = true; queue[qn++] = s; out[n++] = s; }
        }
        int target = 6 + (int)rng.below((u32)(cfg.region_max - 6));
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
                if (inr[ns] || IS_CLUE_CELL[ns]) continue;
                inr[ns] = true;
                queue[qn++] = ns;
                out[n++] = ns;
                grew = true;
            }
            if (!grew) { queue[qi] = queue[--qn]; }
        }
        return n;
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
            nbr[j][0] = cur.q(s - W)[2];
            nbr[j][1] = cur.q(s + 1)[3];
            nbr[j][2] = cur.q(s + W)[0];
            nbr[j][3] = cur.q(s - 1)[1];
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
        if (after > before || (after == before && rng.below(100) < 30)) {
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
                if (cur_score > g_best_score.load(std::memory_order_relaxed))
                    report_completion(cur);
                else if (cur_score + cfg.seed_slack >= g_best_score.load())
                    g_seeds.add(cur);
            }
        }
    }

    int dbg = 0;  // 1: trace moves

    void step() {
        if (!ensure_board()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return;
        }
        g_polish_iters.fetch_add(1, std::memory_order_relaxed);
        if (rng.below(100) < 50) {
            scatter_move();
            if (++since_gain > cfg.stagnation_kick) { since_gain = 0; cur_score = -1; }
            return;
        }
        int cells[40];
        int n = pick_region(cells);
        if (n < 2) { cur_score = -1; return; }

        // score of edges incident to region before; skip fault-free windows
        Board backup = cur;
        auto [before, total_edges] = incident_score_and_edges(cur, cells, n);
        if (before == total_edges) return;  // nothing to gain here
        int gain = rs.prep_and_solve(cur, cells, n, cfg.region_node_cap, rng.next());
        if (dbg) printf("bnb n=%d before=%d/%d gain=%d nodes=%llu\n", n, before,
                        total_edges, gain, (unsigned long long)rs.nodes);
        if (gain > before || (gain == before && rng.below(100) < 30)) {
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
                if (cur_score > g_best_score.load(std::memory_order_relaxed))
                    report_completion(cur);
                else if (cur_score + cfg.seed_slack >= g_best_score.load())
                    g_seeds.add(cur);
            }
        } else {
            cur = backup;  // reject
        }
        if (++since_gain > cfg.stagnation_kick) {
            since_gain = 0;
            cur_score = -1;  // resample a seed
        }
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

static bool load_board_edges(const std::string& edges, Board& b) {
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

// ------------------------------------------------------------------ threads & main
static void hunter_thread(u64 seed) {
    Hunter h(seed);
    while (!g_stop.load()) h.run_restart();
}

static void polisher_thread(u64 seed) {
    Polisher p(seed);
    while (!g_stop.load()) p.step();
}

static void mix_thread(u64 seed) {
    Hunter h(seed);
    Polisher p(seed ^ 0xabcdef123456ULL);
    while (!g_stop.load()) {
        if (g_fresh.size() < 8) { h.greedy_complete(); continue; }
        if (p.rng.uniform() < cfg.polish_frac) {
            for (int i = 0; i < 400 && !g_stop.load(); i++) p.step();
        } else {
            h.run_restart();
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
        printf("[%6llds] best=%d/480 restarts=%llu avgdepth=%llu completions=%llu "
               "nodes=%.2fB (%.1fM/s) polish=%llu(+%llu) seeds=%zu fresh=%zu\n",
               (long long)el, g_best_score.load(), (unsigned long long)rs,
               (unsigned long long)(rs ? g_depth_sum.load() / rs : 0),
               (unsigned long long)g_completions.load(), n / 1e9,
               (n - last_nodes) / 15.0e6, (unsigned long long)g_polish_iters.load(),
               (unsigned long long)g_polish_gains.load(), g_seeds.size(), g_fresh.size());
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
        else if (a == "--region-max") cfg.region_max = std::stoi(next());
        else if (a == "--region-nodes") cfg.region_node_cap = std::stoull(next());
        else if (a == "--polish-frac") cfg.polish_frac = std::stod(next());
        else if (a == "--seed-edges") cfg.seed_url = next();
        else { printf("unknown arg %s\n", a.c_str()); return 1; }
    }
    if (cfg.threads <= 0) {
        int hc = (int)std::thread::hardware_concurrency();
        cfg.threads = std::max(1, hc - 2);
    }
    init_static(cfg.clues);
    init_ring_cells();
    fs::create_directories(cfg.out_dir);

    printf("eternity2 solver | clues=%d threads=%d mode=%s out=%s\n",
           cfg.clues, cfg.threads, cfg.mode.c_str(), cfg.out_dir.c_str());

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
        else if (cfg.mode == "polish") ts.emplace_back(polisher_thread, sd);
        else ts.emplace_back(mix_thread, sd);
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
    printf("final best %d/480\n", g_best_score.load());
    return 0;
}

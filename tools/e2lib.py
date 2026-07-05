"""Eternity II core library.

Ground truth comes from reference/bucas_init.js (e2.bucas.name viewer source):
  - E2_EDGES2PIECES: a full-board arrangement (Louis Verhaard's 467) pairing a
    1024-char edge string (standard motif letters) with official piece numbers.
    This defines every piece's edges + official numbering.
  - E2_PIECES / _JEF / _JBLACKWOOD: sorted canonical piece lists in three motif
    letter orders; used to derive letter permutations between orders.
  - puzzles_urls: reference solutions (467/468/469/470) and the "Clues" preset
    which pins the 5 clue pieces (jef letter order).

Board encoding (bucas): 16x16 cells row-major, 4 letters per cell in order
Up, Right, Down, Left; 'a' = grey/border, 'b'..'w' = motifs 1..22.
Score = matched internal edges, max 480 = 2*16*16 - 32.
Rotation: one clockwise step maps quad "URDL" -> last char to front.
"""

import re
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INIT_JS = os.path.join(ROOT, "reference", "bucas_init.js")

W = H = 16
N_CELLS = W * H
MAX_SCORE = 2 * W * H - W - H  # 480


def _read_init_js():
    with open(INIT_JS, "r", encoding="utf-8") as f:
        return f.read()


def _extract_var(js, name):
    m = re.search(name + r'\s*=\s*"([^"]+)"', js)
    return m.group(1)


def rot_quad(q, k):
    """Rotate a 4-char URDL quad clockwise k times (last char to front)."""
    k %= 4
    for _ in range(k):
        q = q[3] + q[:3]
    return q


def canon(q):
    return min(rot_quad(q, k) for k in range(4))


class E2Data:
    def __init__(self):
        js = _read_init_js()
        m = re.search(r'E2_EDGES2PIECES\s*=\s*\[\s*"([a-w]+)",\s*"(\d+)"\s*\]', js)
        edges, nums = m.group(1), m.group(2)
        assert len(edges) == N_CELLS * 4 and len(nums) == N_CELLS * 3
        # piece number -> quad (URDL, standard letters) in the arrangement's orientation
        self.piece_quad = {}
        for s in range(N_CELLS):
            p = int(nums[s * 3:s * 3 + 3])
            self.piece_quad[p] = edges[s * 4:s * 4 + 4]
        assert sorted(self.piece_quad) == list(range(1, 257))
        self.canon_to_piece = {}
        for p, q in self.piece_quad.items():
            c = canon(q)
            assert c not in self.canon_to_piece, "duplicate piece shape"
            self.canon_to_piece[c] = p

        self.pieces_std = _extract_var(js, "E2_PIECES").split(",")
        self.pieces_jef = _extract_var(js, "E2_PIECES_MOTIFS_ORDER_JEF").split(",")
        self.pieces_jbw = _extract_var(js, "E2_PIECES_MOTIFS_ORDER_JBLACKWOOD").split(",")
        assert sorted(self.pieces_std) == sorted(canon(q) for q in self.piece_quad.values())

        self.jef_to_std = derive_letter_map(self.pieces_jef, self.pieces_std)
        self.jbw_to_std = derive_letter_map(self.pieces_jbw, self.pieces_std)

        self.puzzles = {}
        for mm in re.finditer(r'puzzles_urls.push\("([^"]+)"\)', js):
            params = dict(kv.split("=", 1) for kv in mm.group(1).split("&"))
            self.puzzles[params["puzzle"]] = params

    def board_to_std(self, edges, motifs_order=None):
        """Translate a 1024-char edge string to standard letters."""
        if motifs_order in (None, "", "standard"):
            return edges
        table = {"jef": self.jef_to_std, "jblackwood": self.jbw_to_std,
                 "marie": self.jbw_to_std}[motifs_order]
        return "".join(table[ch] for ch in edges)

    def identify(self, quad):
        """Return (piece_number, rotation) for a standard-letter URDL quad."""
        p = self.canon_to_piece.get(canon(quad))
        if p is None:
            return None, None
        base = self.piece_quad[p]
        for k in range(4):
            if rot_quad(base, k) == quad:
                return p, k
        return None, None


def derive_letter_map(src_list, dst_list):
    """Derive letter permutation mapping src motif order onto dst, using the
    sorted canonical piece lists. Refinement on letter-frequency signatures."""
    src_txt = "".join(src_list)
    dst_txt = "".join(dst_list)
    letters_src = sorted(set(src_txt))
    letters_dst = sorted(set(dst_txt))
    assert len(letters_src) == len(letters_dst) == 23

    # signature: (total count, sorted per-quad count profile)
    def sigs(lst, letters):
        base = {}
        for ch in letters:
            per_quad = Counter()
            tot = 0
            for q in lst:
                c = q.count(ch)
                if c:
                    per_quad[c] += 1
                    tot += c
            base[ch] = (tot, tuple(sorted(per_quad.items())))
        return base

    s_sig, d_sig = sigs(src_list, letters_src), sigs(dst_list, letters_dst)
    # group by signature
    from collections import defaultdict
    groups = defaultdict(lambda: ([], []))
    for ch, sg in s_sig.items():
        groups[sg][0].append(ch)
    for ch, sg in d_sig.items():
        groups[sg][1].append(ch)

    mapping = {}
    ambiguous = []
    for sg, (ss, dd) in groups.items():
        assert len(ss) == len(dd), f"signature mismatch {sg}: {ss} vs {dd}"
        if len(ss) == 1:
            mapping[ss[0]] = dd[0]
        else:
            ambiguous.append((ss, dd))

    # resolve all ambiguous groups jointly by brute force over the product of
    # per-group permutations, verified against the full piece multiset
    if ambiguous:
        import itertools
        dst_sorted = sorted(dst_list)
        total = 1
        for ss, dd in ambiguous:
            for _ in range(2, len(ss) + 1):
                total *= _
        assert total <= 10_000_000, f"too many permutations to brute force: {total}"
        found = False
        for combo in itertools.product(*(itertools.permutations(dd) for _, dd in ambiguous)):
            trial = dict(mapping)
            for (ss, _), perm in zip(ambiguous, combo):
                trial.update(dict(zip(ss, perm)))
            tr = str.maketrans(trial)
            if sorted(canon(q.translate(tr)) for q in src_list) == dst_sorted:
                mapping = trial
                found = True
                break
        assert found, "no permutation satisfies the piece multiset"

    assert len(mapping) == 23
    tr = str.maketrans(mapping)
    assert sorted(canon(q.translate(tr)) for q in src_list) == sorted(dst_list), \
        "letter map verification failed"
    return mapping


def score_edges(edges):
    """Count matched internal edges of a std-letter 1024-char board string.
    Returns (score, conflicts, border_errors). Matching grey ('a') internal
    edges count as conflicts, same as the viewer. Border errors = non-grey on
    the outer rim (not part of score, but must be 0 for a proper solution)."""
    assert len(edges) == N_CELLS * 4
    UP, RIGHT, DOWN, LEFT = 0, 1, 2, 3
    conflicts = 0
    for y in range(H):
        for x in range(W):
            s = x + y * W
            if x != W - 1:
                a = edges[s * 4 + RIGHT]
                b = edges[(s + 1) * 4 + LEFT]
                if a != b or a == "a":
                    conflicts += 1
            if y != H - 1:
                a = edges[s * 4 + DOWN]
                b = edges[(s + W) * 4 + UP]
                if a != b or a == "a":
                    conflicts += 1
    border_errors = 0
    for x in range(W):
        if edges[x * 4 + UP] != "a":
            border_errors += 1
        if edges[(x + (H - 1) * W) * 4 + DOWN] != "a":
            border_errors += 1
    for y in range(H):
        if edges[(y * W) * 4 + LEFT] != "a":
            border_errors += 1
        if edges[(y * W + W - 1) * 4 + RIGHT] != "a":
            border_errors += 1
    return MAX_SCORE - conflicts, conflicts, border_errors


def check_piece_set(data, edges):
    """Verify the board uses each of the 256 pieces exactly once.
    Returns list of (piece, rotation) per cell."""
    placements = []
    seen = Counter()
    for s in range(N_CELLS):
        q = edges[s * 4:s * 4 + 4]
        p, r = data.identify(q)
        if p is None:
            raise ValueError(f"cell {s}: quad {q} is not an E2 piece")
        placements.append((p, r))
        seen[p] += 1
    dups = [p for p, c in seen.items() if c > 1]
    if dups:
        raise ValueError(f"duplicated pieces: {dups}")
    return placements


def build_url(edges, name="Eternity2", include_pieces=True, data=None):
    url = (f"https://e2.bucas.name/#puzzle={name}&board_w=16&board_h=16"
           f"&board_edges={edges}")
    if include_pieces and data is not None:
        placements = check_piece_set(data, edges)
        url += "&board_pieces=" + "".join(f"{p:03d}" for p, _ in placements)
    return url


def parse_url(url):
    """Extract params from a bucas URL or query/hash string."""
    frag = url.split("#", 1)[-1]
    params = dict(kv.split("=", 1) for kv in frag.split("&") if "=" in kv)
    return params

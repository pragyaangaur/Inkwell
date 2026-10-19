/* chess: play chess against Inkwell's own engine. The engine is a small
 * alpha-beta search (iterative deepening, quiescence, killer moves, Zobrist
 * keys for repetitions) over a 10x12 mailbox board, with piece-square tables
 * for evaluation. Easy, Medium and Hard differ in search time, depth and how
 * much noise is added to its choices. Tap a piece to see where it can go,
 * then tap a square. The game is saved to /mnt/us/chess after every move. */
#include "ui.h"
#include <sys/stat.h>
#include <time.h>

#define SAVE_DIR "/mnt/us/chess"
#define SAVE_FILE SAVE_DIR "/game.txt"

/* ---- the board ----------------------------------------------------------------- */

/* Squares 21..98 of a 10x12 board, row 0 is rank 8. White pieces are
 * positive, black negative, and OFF marks the border. */
enum { EMPTY = 0, P = 1, N, B, R, Q, K, OFF = 99 };
enum { F_CAP = 1, F_EP = 2, F_CASTLE = 4, F_DOUBLE = 8 };
typedef struct { unsigned char from, to, promo, flags; } move;
typedef struct { move m; int cap, castle, ep, half; unsigned long long zkey; } undo;

static int bd[120], side = 1, castle, ep, half, kpos[2];
static undo hist[1200];
static int nhist;
static unsigned long long zkey, zob[13][120], zside, zcastle[16], zep[120];
static int cmask[120];

static const int N_OFF[8] = {-21, -19, -12, -8, 8, 12, 19, 21};
static const int K_OFF[8] = {-11, -10, -9, -1, 1, 9, 10, 11};
static const int DIAG[4] = {-11, -9, 9, 11}, ORTH[4] = {-10, -1, 1, 10};
static const int VAL[7] = {0, 100, 320, 330, 500, 900, 0};

static int iabs(int v) { return v < 0 ? -v : v; }
static int kidx(int s) { return s == 1 ? 0 : 1; }

static unsigned long long rng = 0x9E3779B97F4A7C15ull;
static unsigned long long rnd64(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void compute_key(void) {
    zkey = 0;
    for (int s = 21; s <= 98; s++) if (bd[s] != OFF && bd[s]) zkey ^= zob[bd[s] + 6][s];
    if (side == -1) zkey ^= zside;
    zkey ^= zcastle[castle];
    if (ep) zkey ^= zep[ep];
}

static void new_position(void) {
    static const int back[8] = {R, N, B, Q, K, B, N, R};
    for (int i = 0; i < 120; i++) bd[i] = OFF;
    for (int r = 0; r < 8; r++) for (int f = 0; f < 8; f++) bd[21 + f + 10 * r] = EMPTY;
    for (int f = 0; f < 8; f++) {
        bd[21 + f] = -back[f]; bd[31 + f] = -P;
        bd[81 + f] = P; bd[91 + f] = back[f];
    }
    side = 1; castle = 15; ep = 0; half = 0; nhist = 0;
    kpos[0] = 95; kpos[1] = 25;
    compute_key();
}

static void init_tables(void) {
    for (int p = 0; p < 13; p++) for (int s = 0; s < 120; s++) zob[p][s] = rnd64();
    zside = rnd64();
    for (int i = 0; i < 16; i++) zcastle[i] = rnd64();
    for (int s = 0; s < 120; s++) zep[s] = rnd64();
    for (int s = 0; s < 120; s++) cmask[s] = 15;
    cmask[95] = 12; cmask[98] = 14; cmask[91] = 13; /* white king, h1 and a1 rooks */
    cmask[25] = 3; cmask[28] = 11; cmask[21] = 7;  /* black king, h8 and a8 rooks */
}

static int attacked(int sq, int by) {
    if (by == 1) { if (bd[sq + 9] == P || bd[sq + 11] == P) return 1; }
    else if (bd[sq - 9] == -P || bd[sq - 11] == -P) return 1;
    for (int i = 0; i < 8; i++) {
        if (bd[sq + N_OFF[i]] == N * by) return 1;
        if (bd[sq + K_OFF[i]] == K * by) return 1;
    }
    for (int i = 0; i < 4; i++) {
        int s = sq + DIAG[i];
        while (bd[s] == EMPTY) s += DIAG[i];
        if (bd[s] == B * by || bd[s] == Q * by) return 1;
        s = sq + ORTH[i];
        while (bd[s] == EMPTY) s += ORTH[i];
        if (bd[s] == R * by || bd[s] == Q * by) return 1;
    }
    return 0;
}
static int in_check(int s) { return attacked(kpos[kidx(s)], -s); }
/* After make(): did the side that just moved leave its king attacked? */
static int left_in_check(void) { return attacked(kpos[kidx(-side)], side); }

static void make(move m) {
    undo *u = &hist[nhist++];
    u->m = m; u->castle = castle; u->ep = ep; u->half = half; u->zkey = zkey;
    int pc = bd[m.from], cap = bd[m.to];
    if (m.flags & F_EP) {
        int cs = m.to + (side == 1 ? 10 : -10);
        cap = bd[cs]; bd[cs] = EMPTY; zkey ^= zob[cap + 6][cs];
    } else if (cap) zkey ^= zob[cap + 6][m.to];
    u->cap = cap;
    zkey ^= zob[pc + 6][m.from];
    bd[m.from] = EMPTY;
    int np = m.promo ? m.promo * side : pc;
    bd[m.to] = np;
    zkey ^= zob[np + 6][m.to];
    if (iabs(pc) == K) kpos[kidx(side)] = m.to;
    if (m.flags & F_CASTLE) {
        int rf = m.to > m.from ? m.from + 3 : m.from - 4, rt = m.to > m.from ? m.from + 1 : m.from - 1;
        int rk = bd[rf];
        bd[rf] = EMPTY; bd[rt] = rk;
        zkey ^= zob[rk + 6][rf] ^ zob[rk + 6][rt];
    }
    zkey ^= zcastle[castle];
    castle &= cmask[m.from] & cmask[m.to];
    zkey ^= zcastle[castle];
    if (ep) zkey ^= zep[ep];
    ep = 0;
    if (m.flags & F_DOUBLE) { ep = (m.from + m.to) / 2; zkey ^= zep[ep]; }
    half = (iabs(pc) == P || cap) ? 0 : half + 1;
    side = -side;
    zkey ^= zside;
}

static void unmake(void) {
    undo *u = &hist[--nhist];
    move m = u->m;
    side = -side;
    int pc = m.promo ? P * side : bd[m.to];
    bd[m.from] = pc;
    if (m.flags & F_EP) { bd[m.to] = EMPTY; bd[m.to + (side == 1 ? 10 : -10)] = u->cap; }
    else bd[m.to] = u->cap;
    if (iabs(pc) == K) kpos[kidx(side)] = m.from;
    if (m.flags & F_CASTLE) {
        int rf = m.to > m.from ? m.from + 3 : m.from - 4, rt = m.to > m.from ? m.from + 1 : m.from - 1;
        bd[rf] = bd[rt]; bd[rt] = EMPTY;
    }
    castle = u->castle; ep = u->ep; half = u->half; zkey = u->zkey;
}

static int enemy(int q) { return q != OFF && q != EMPTY && (q > 0) != (side > 0); }

static void add(move *l, int *n, int from, int to, int promo, int flags) {
    l[(*n)++] = (move){(unsigned char)from, (unsigned char)to, (unsigned char)promo, (unsigned char)flags};
}
static void add_pawn(move *l, int *n, int from, int to, int flags, int caps_only) {
    int row = to / 10 - 2;
    if (row == 0 || row == 7) {
        add(l, n, from, to, Q, flags);
        if (!caps_only) { add(l, n, from, to, R, flags); add(l, n, from, to, B, flags); add(l, n, from, to, N, flags); }
    } else add(l, n, from, to, 0, flags);
}

/* Pseudo-legal moves for the side to move. caps_only gives captures and
 * queen promotions, for the quiescence search. */
static int gen(move *l, int caps_only) {
    int n = 0;
    for (int sq = 21; sq <= 98; sq++) {
        int p = bd[sq];
        if (p == OFF || p == EMPTY || (p > 0) != (side > 0)) continue;
        int t = iabs(p);
        if (t == P) {
            int dir = side == 1 ? -10 : 10, row = sq / 10 - 2, to = sq + dir;
            if (bd[to] == EMPTY) {
                int prow = to / 10 - 2;
                if (!caps_only || prow == 0 || prow == 7) add_pawn(l, &n, sq, to, 0, caps_only);
                if (!caps_only && ((side == 1 && row == 6) || (side == -1 && row == 1)) && bd[to + dir] == EMPTY)
                    add(l, &n, sq, to + dir, 0, F_DOUBLE);
            }
            for (int c = -1; c <= 1; c += 2) {
                int cs = to + c;
                if (enemy(bd[cs])) add_pawn(l, &n, sq, cs, F_CAP, caps_only);
                else if (ep && cs == ep) add(l, &n, sq, cs, 0, F_CAP | F_EP);
            }
        } else if (t == N || t == K) {
            const int *o = t == N ? N_OFF : K_OFF;
            for (int i = 0; i < 8; i++) {
                int to = sq + o[i], q = bd[to];
                if (q == EMPTY) { if (!caps_only) add(l, &n, sq, to, 0, 0); }
                else if (enemy(q)) add(l, &n, sq, to, 0, F_CAP);
            }
        } else {
            for (int k = 0; k < 2; k++) {
                if ((k == 0 && t == R) || (k == 1 && t == B)) continue;
                const int *d = k == 0 ? DIAG : ORTH;
                for (int i = 0; i < 4; i++)
                    for (int to = sq + d[i];; to += d[i]) {
                        int q = bd[to];
                        if (q == EMPTY) { if (!caps_only) add(l, &n, sq, to, 0, 0); continue; }
                        if (enemy(q)) add(l, &n, sq, to, 0, F_CAP);
                        break;
                    }
            }
        }
    }
    if (!caps_only) {
        if (side == 1 && bd[95] == K) {
            if ((castle & 1) && bd[96] == EMPTY && bd[97] == EMPTY && !attacked(95, -1) && !attacked(96, -1) && !attacked(97, -1))
                add(l, &n, 95, 97, 0, F_CASTLE);
            if ((castle & 2) && bd[94] == EMPTY && bd[93] == EMPTY && bd[92] == EMPTY && !attacked(95, -1) && !attacked(94, -1) && !attacked(93, -1))
                add(l, &n, 95, 93, 0, F_CASTLE);
        } else if (side == -1 && bd[25] == -K) {
            if ((castle & 4) && bd[26] == EMPTY && bd[27] == EMPTY && !attacked(25, 1) && !attacked(26, 1) && !attacked(27, 1))
                add(l, &n, 25, 27, 0, F_CASTLE);
            if ((castle & 8) && bd[24] == EMPTY && bd[23] == EMPTY && bd[22] == EMPTY && !attacked(25, 1) && !attacked(24, 1) && !attacked(23, 1))
                add(l, &n, 25, 23, 0, F_CASTLE);
        }
    }
    return n;
}

static int legal_moves(move *out) {
    move l[256];
    int n = gen(l, 0), k = 0;
    for (int i = 0; i < n; i++) {
        make(l[i]);
        if (!left_in_check()) out[k++] = l[i];
        unmake();
    }
    return k;
}

/* ---- evaluation -------------------------------------------------------------- */

/* Tomasz Michniewski's simplified evaluation tables, from White's side with
 * rank 8 first. Black reads them upside down. */
static const signed char PST[7][64] = {
    {0},
    {0, 0, 0, 0, 0, 0, 0, 0, 50, 50, 50, 50, 50, 50, 50, 50, 10, 10, 20, 30, 30, 20, 10, 10, 5, 5, 10, 25, 25, 10, 5, 5,
     0, 0, 0, 20, 20, 0, 0, 0, 5, -5, -10, 0, 0, -10, -5, 5, 5, 10, 10, -20, -20, 10, 10, 5, 0, 0, 0, 0, 0, 0, 0, 0},
    {-50, -40, -30, -30, -30, -30, -40, -50, -40, -20, 0, 0, 0, 0, -20, -40, -30, 0, 10, 15, 15, 10, 0, -30, -30, 5, 15, 20, 20, 15, 5, -30,
     -30, 0, 15, 20, 20, 15, 0, -30, -30, 5, 10, 15, 15, 10, 5, -30, -40, -20, 0, 5, 5, 0, -20, -40, -50, -40, -30, -30, -30, -30, -40, -50},
    {-20, -10, -10, -10, -10, -10, -10, -20, -10, 0, 0, 0, 0, 0, 0, -10, -10, 0, 5, 10, 10, 5, 0, -10, -10, 5, 5, 10, 10, 5, 5, -10,
     -10, 0, 10, 10, 10, 10, 0, -10, -10, 10, 10, 10, 10, 10, 10, -10, -10, 5, 0, 0, 0, 0, 5, -10, -20, -10, -10, -10, -10, -10, -10, -20},
    {0, 0, 0, 0, 0, 0, 0, 0, 5, 10, 10, 10, 10, 10, 10, 5, -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5, 0, 0, 0, 5, 5, 0, 0, 0},
    {-20, -10, -10, -5, -5, -10, -10, -20, -10, 0, 0, 0, 0, 0, 0, -10, -10, 0, 5, 5, 5, 5, 0, -10, -5, 0, 5, 5, 5, 5, 0, -5,
     0, 0, 5, 5, 5, 5, 0, -5, -10, 5, 5, 5, 5, 5, 0, -10, -10, 0, 5, 0, 0, 0, 0, -10, -20, -10, -10, -5, -5, -10, -10, -20},
    {-30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
     -20, -30, -30, -40, -40, -30, -30, -20, -10, -20, -20, -20, -20, -20, -20, -10, 20, 20, 0, 0, 0, 0, 20, 20, 20, 30, 10, 0, 0, 10, 30, 20},
};
static const signed char KING_END[64] = {
    -50, -40, -30, -20, -20, -30, -40, -50, -30, -20, -10, 0, 0, -10, -20, -30, -30, -10, 20, 30, 30, 20, -10, -30, -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30, -30, -10, 20, 30, 30, 20, -10, -30, -30, -30, 0, 0, 0, 0, -30, -30, -50, -30, -30, -30, -30, -30, -30, -50};

static int idx64(int sq, int white) { int r = sq / 10 - 2, f = sq % 10 - 1; return (white ? r : 7 - r) * 8 + f; }

/* The score for the side to move, in centipawns. */
static int eval(void) {
    int s = 0, mat = 0, bishops[2] = {0, 0};
    for (int sq = 21; sq <= 98; sq++) {
        int p = bd[sq];
        if (p == OFF || p == EMPTY) continue;
        int t = iabs(p), w = p > 0, sg = w ? 1 : -1;
        if (t == K) continue;
        if (t != P) mat += VAL[t];
        if (t == B) bishops[!w]++;
        s += sg * (VAL[t] + PST[t][idx64(sq, w)]);
    }
    const signed char *kt = mat < 1400 ? KING_END : PST[K];
    s += kt[idx64(kpos[0], 1)] - kt[idx64(kpos[1], 0)];
    if (bishops[0] >= 2) s += 30;
    if (bishops[1] >= 2) s -= 30;
    return s * side;
}

/* ---- search -------------------------------------------------------------------- */

#define INF 1000000
#define MATE 100000
static long nodes;
static double t_end;
static int stop_search;
static move killers[128][2];

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void tick(void) {
    if ((++nodes & 1023) == 0 && now_s() > t_end) stop_search = 1;
}

static int same(move a, move b) { return a.from == b.from && a.to == b.to && a.promo == b.promo; }

static void order(move *l, int n, int ply) {
    int sc[256];
    for (int i = 0; i < n; i++) {
        move m = l[i];
        int s = 0;
        if (m.flags & F_CAP) {
            int v = m.flags & F_EP ? P : iabs(bd[m.to]);
            s = 100000 + VAL[v] * 10 - VAL[iabs(bd[m.from])] / 10;
        } else if (ply < 128 && same(m, killers[ply][0])) s = 90000;
        else if (ply < 128 && same(m, killers[ply][1])) s = 89000;
        if (m.promo) s += 80000 + VAL[m.promo];
        sc[i] = s;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && sc[j - 1] < sc[j]; j--) {
            int t = sc[j]; sc[j] = sc[j - 1]; sc[j - 1] = t;
            move tm = l[j]; l[j] = l[j - 1]; l[j - 1] = tm;
        }
}

static int repeated(void) {
    for (int i = nhist - 2; i >= 0 && i >= nhist - half; i -= 2)
        if (hist[i].zkey == zkey) return 1;
    return 0;
}

static int quiesce(int alpha, int beta, int ply) {
    tick();
    if (stop_search) return 0;
    int stand = eval();
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    if (ply > 100) return stand;
    move l[256];
    int n = gen(l, 1);
    order(l, n, ply);
    for (int i = 0; i < n; i++) {
        make(l[i]);
        if (left_in_check()) { unmake(); continue; }
        int s = -quiesce(-beta, -alpha, ply + 1);
        unmake();
        if (stop_search) return 0;
        if (s >= beta) return s;
        if (s > alpha) alpha = s;
    }
    return alpha;
}

static int search(int depth, int alpha, int beta, int ply) {
    if (half >= 100 || repeated()) return 0;
    int chk = in_check(side);
    if (chk && ply < 60) depth++;
    if (depth <= 0) return quiesce(alpha, beta, ply);
    tick();
    if (stop_search) return 0;
    move l[256];
    int n = gen(l, 0), legal = 0, best = -INF;
    order(l, n, ply);
    for (int i = 0; i < n; i++) {
        make(l[i]);
        if (left_in_check()) { unmake(); continue; }
        legal++;
        int s = -search(depth - 1, -beta, -alpha, ply + 1);
        unmake();
        if (stop_search) return 0;
        if (s > best) best = s;
        if (s > alpha) alpha = s;
        if (alpha >= beta) {
            if (!(l[i].flags & F_CAP) && ply < 128 && !same(l[i], killers[ply][0])) {
                killers[ply][1] = killers[ply][0];
                killers[ply][0] = l[i];
            }
            break;
        }
    }
    if (!legal) return chk ? -MATE + ply : 0;
    return best;
}

typedef struct { const char *name; int depth; double secs; int noise; } level;
static const level LEVELS[3] = {{"Easy", 2, 0.6, 90}, {"Medium", 5, 2.0, 12}, {"Hard", 40, 6.0, 0}};

/* The engine's move for the side to move, by iterative deepening. */
static move think(int lv) {
    const level *L = &LEVELS[lv];
    move root[256];
    int n = legal_moves(root);
    move best = root[0];
    double t0 = now_s();
    t_end = t0 + L->secs;
    stop_search = 0;
    nodes = 0;
    memset(killers, 0, sizeof killers);
    order(root, n, 0);
    for (int d = 1; d <= L->depth; d++) {
        int alpha = -INF, cur = 0;
        for (int i = 0; i < n; i++) {
            make(root[i]);
            /* with noise every move needs its exact score, so no narrow window */
            int s = -search(d - 1, -INF, L->noise ? INF : -alpha, 1);
            unmake();
            if (stop_search) break;
            if (L->noise) s += rand() % (2 * L->noise + 1) - L->noise;
            if (s > alpha) { alpha = s; cur = i; }
        }
        if (stop_search && d > 1) break;
        /* the best move goes first, so the next, deeper pass searches it first */
        move m = root[cur];
        memmove(root + 1, root, sizeof *root * cur);
        root[0] = m;
        best = m;
        if (alpha > MATE - 200 || now_s() - t0 > L->secs / 2) break;
    }
    return best;
}

/* ---- notation ---------------------------------------------------------------- */

static void sq_name(int sq, char *o) { o[0] = 'a' + sq % 10 - 1; o[1] = '0' + 8 - (sq / 10 - 2); o[2] = 0; }

static const char PIECE_CH[7] = {0, 'P', 'N', 'B', 'R', 'Q', 'K'};

/* Standard algebraic notation, worked out before the move is made. */
static void san(move m, char *out) {
    char a[3], b[3];
    int t = iabs(bd[m.from]), k = 0;
    sq_name(m.from, a);
    sq_name(m.to, b);
    if (m.flags & F_CASTLE) k = sprintf(out, m.to > m.from ? "O-O" : "O-O-O");
    else {
        if (t == P) {
            if (m.flags & F_CAP) out[k++] = a[0];
        } else {
            out[k++] = PIECE_CH[t];
            move l[256];
            int n = legal_moves(l), file = 0, rank = 0, other = 0;
            for (int i = 0; i < n; i++) {
                if (l[i].to != m.to || l[i].from == m.from || iabs(bd[l[i].from]) != t) continue;
                other = 1;
                if (l[i].from % 10 == m.from % 10) file = 1;
                if (l[i].from / 10 == m.from / 10) rank = 1;
            }
            if (other) {
                if (!file) out[k++] = a[0];
                else if (!rank) out[k++] = a[1];
                else { out[k++] = a[0]; out[k++] = a[1]; }
            }
        }
        if (m.flags & F_CAP) out[k++] = 'x';
        out[k++] = b[0]; out[k++] = b[1];
        if (m.promo) { out[k++] = '='; out[k++] = PIECE_CH[m.promo]; }
    }
    make(m);
    if (in_check(side)) {
        move l[256];
        out[k++] = legal_moves(l) ? '+' : '#';
    }
    unmake();
    out[k] = 0;
}

/* ---- the game ---------------------------------------------------------------- */

static int player = 1, lv = 1, selected, last_from, last_to;
static char status[160];
static int over;

/* Why the game is over, or NULL. Also notes check. */
static const char *game_result(void) {
    move l[256];
    if (!legal_moves(l)) {
        if (!in_check(side)) return "Stalemate. It's a draw.";
        return side == player ? "Checkmate. Inkwell wins." : "Checkmate. You win!";
    }
    if (half >= 100) return "Draw by the fifty-move rule.";
    int reps = 0;
    for (int i = nhist - 2; i >= 0 && i >= nhist - half; i -= 2) if (hist[i].zkey == zkey) reps++;
    if (reps >= 2) return "Draw by repetition.";
    int minor = 0, other = 0;
    for (int s = 21; s <= 98; s++) {
        int t = bd[s] == OFF ? 0 : iabs(bd[s]);
        if (t == N || t == B) minor++;
        else if (t == P || t == R || t == Q) other++;
    }
    if (!other && minor <= 1) return "Draw. Neither side can mate.";
    return 0;
}

static void save_game(void) {
    mkdir(SAVE_DIR, 0755);
    FILE *f = fopen(SAVE_FILE, "w");
    if (!f) return;
    fprintf(f, "%d %d\n", player, lv);
    for (int i = 0; i < nhist; i++) {
        char a[3], b[3];
        sq_name(hist[i].m.from, a);
        sq_name(hist[i].m.to, b);
        fprintf(f, "%s%s%s ", a, b, hist[i].m.promo ? (char[]){PIECE_CH[hist[i].m.promo] + 32, 0} : "");
    }
    fputc('\n', f);
    fclose(f);
}

static int parse_sq(const char *s) {
    if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return 0;
    return 21 + (s[0] - 'a') + 10 * (8 - (s[1] - '0'));
}

static void load_game(void) {
    new_position();
    FILE *f = fopen(SAVE_FILE, "r");
    if (!f) return;
    int p, l;
    if (fscanf(f, "%d %d", &p, &l) == 2) { player = p == -1 ? -1 : 1; lv = l >= 0 && l < 3 ? l : 1; }
    char tok[8];
    while (fscanf(f, "%7s", tok) == 1) {
        int from = parse_sq(tok), to = parse_sq(tok + 2);
        int promo = tok[4] == 'n' ? N : tok[4] == 'b' ? B : tok[4] == 'r' ? R : tok[4] == 'q' ? Q : 0;
        move ms[256];
        int n = legal_moves(ms), ok = 0;
        for (int i = 0; i < n; i++)
            if (ms[i].from == from && ms[i].to == to && ms[i].promo == promo) { make(ms[i]); ok = 1; break; }
        if (!ok) break;
    }
    fclose(f);
    if (nhist) { last_from = hist[nhist - 1].m.from; last_to = hist[nhist - 1].m.to; }
}

/* ---- drawing ------------------------------------------------------------------- */

#define SQ 128
#define BX ((W - 8 * SQ) / 2)
#define BY 176
#define STATUS_Y (BY + 8 * SQ + 26)
#define BUTTONS_Y 1316
#define BUTTONS_H 96
#define PIECE_PX 112

static rect btn_new, btn_undo, btn_level, btn_side;

static void sq_xy(int sq, int *x, int *y) {
    int r = sq / 10 - 2, f = sq % 10 - 1;
    if (player == -1) { r = 7 - r; f = 7 - f; }
    *x = BX + f * SQ; *y = BY + r * SQ;
}
static int sq_at(int x, int y) {
    if (x < BX || x >= BX + 8 * SQ || y < BY || y >= BY + 8 * SQ) return 0;
    int f = (x - BX) / SQ, r = (y - BY) / SQ;
    if (player == -1) { r = 7 - r; f = 7 - f; }
    return 21 + f + 10 * r;
}

/* A glyph from the chess font sitting on a common baseline in its square. */
static void piece_glyph(int cp, int x, int y, unsigned char ink) {
    const glyph *g = glyph_get(&f_chess, PIECE_PX, cp);
    int base = y + SQ * 82 / 100, x0 = x + SQ / 2 - (g->ox + g->w / 2);
    for (int j = 0; j < g->h; j++)
        for (int i = 0; i < g->w; i++) {
            int a = g->bm[j * g->w + i];
            if (a) blend(x0 + g->ox + i, base + g->oy + j, ink, a / 255.0f);
        }
}
static void draw_piece(int p, int x, int y) {
    static const int ofs[7] = {0, 5, 4, 3, 2, 1, 0}; /* P N B R Q K in Unicode order K Q R B N P */
    int t = iabs(p);
    if (p > 0) {
        piece_glyph(0x265A + ofs[t], x, y, WHITE);  /* fill the outline white */
        piece_glyph(0x2654 + ofs[t], x, y, BLACK);
    } else piece_glyph(0x265A + ofs[t], x, y, BLACK);
}

static void draw_board(void) {
    move l[256];
    int n = selected ? legal_moves(l) : 0;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int sq = 21 + f + 10 * r, x, y;
            sq_xy(sq, &x, &y);
            int dark = (r + f) & 1;
            fill(x, y, SQ, SQ, dark ? 0xB0 : WHITE);
            if (sq == last_from || sq == last_to) frame(x + 3, y + 3, SQ - 6, SQ - 6, 5, 0x50);
            if (sq == selected) frame(x, y, SQ, SQ, 9, BLACK);
            if (bd[sq]) draw_piece(bd[sq], x, y);
            for (int i = 0; i < n; i++)
                if (l[i].from == selected && l[i].to == sq) {
                    if (bd[sq]) ring(x + SQ / 2.0f, y + SQ / 2.0f, SQ / 2.0f - 8, 6, BLACK);
                    else disc(x + SQ / 2.0f, y + SQ / 2.0f, 15, BLACK);
                    break;
                }
        }
    /* file letters along the bottom edge and rank numbers down the left */
    for (int i = 0; i < 8; i++) {
        char fl[2] = {(char)('a' + (player == 1 ? i : 7 - i)), 0}, rk[2] = {(char)('0' + (player == 1 ? 8 - i : i + 1)), 0};
        int bottom_dark = (7 + (player == 1 ? i : 7 - i)) & 1, left_dark = (i + (player == 1 ? 0 : 7)) & 1;
        text(&f_ui_bold, 22, BX + i * SQ + SQ - 20, BY + 8 * SQ - 28, fl, bottom_dark ? WHITE : 0x70);
        text(&f_ui_bold, 22, BX + 6, BY + i * SQ + 4, rk, left_dark ? WHITE : 0x70);
    }
    frame(BX - 3, BY - 3, 8 * SQ + 6, 8 * SQ + 6, 3, BLACK);
}

static void draw_status(void) {
    fill(0, STATUS_Y - 6, W, 70, WHITE);
    text_center(&f_ui_bold, 38, W / 2, STATUS_Y, status, BLACK);
}

static void draw_buttons(void) {
    int bw = (W - 2 * MARGIN - 3 * 20) / 4;
    btn_new = (rect){MARGIN, BUTTONS_Y, bw, BUTTONS_H};
    btn_undo = (rect){MARGIN + bw + 20, BUTTONS_Y, bw, BUTTONS_H};
    btn_level = (rect){MARGIN + 2 * (bw + 20), BUTTONS_Y, bw, BUTTONS_H};
    btn_side = (rect){MARGIN + 3 * (bw + 20), BUTTONS_Y, bw, BUTTONS_H};
    fill(0, BUTTONS_Y - 10, W, BUTTONS_H + 20, WHITE);
    pill(btn_new, "New game", 1);
    pill(btn_undo, "Undo", 0);
    pill(btn_level, LEVELS[lv].name, 0);
    pill(btn_side, player == 1 ? "Play black" : "Play white", 0);
}

static void draw_page(void) {
    header("Chess");
    draw_board();
    draw_status();
    draw_buttons();
}

static void refresh_board(void) { refresh(BX - 4, BY - 4, 8 * SQ + 8, 8 * SQ + 8, WF_GC16, 0, 0); }
static void refresh_status(void) { refresh(0, STATUS_Y - 6, W, 70, WF_GC16, 0, 0); }

static void set_turn_status(const char *played) {
    const char *res = game_result();
    over = res != 0;
    if (res && played) snprintf(status, sizeof status, "%s %s", played, res);
    else if (res) snprintf(status, sizeof status, "%s", res);
    else if (side != player) snprintf(status, sizeof status, "Inkwell is thinking...");
    else if (played) snprintf(status, sizeof status, "%s %s", played, in_check(side) ? "You are in check." : "Your move.");
    else snprintf(status, sizeof status, in_check(side) ? "You are in check." : "Your move.");
}

static void engine_move(void) {
    set_turn_status(0);
    draw_status();
    refresh_status();
    move m = think(lv);
    char s[16], msg[64];
    san(m, s);
    make(m);
    last_from = m.from; last_to = m.to;
    snprintf(msg, sizeof msg, "Inkwell played %s.", s);
    set_turn_status(msg);
    save_game();
    draw_board();
    draw_status();
    refresh(0, BY - 4, W, STATUS_Y + 64 - BY + 4, WF_GC16, 0, 0);
    input_drain(); /* taps made while the engine thought don't count */
}

static void start_new(void) {
    new_position();
    selected = last_from = last_to = 0;
    save_game();
    set_turn_status(0);
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    init_tables();
    srand((unsigned)time(0));
    load_game();
    set_turn_status(0);
    draw_page();
    flip_flash();
    if (!over && side != player) engine_move();

    for (;;) {
        event e = input_wait(-1);
        if (e.type == EV_POWER) break;
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(btn_new, e.x, e.y)) press_on(btn_new);
            else if (hit(btn_undo, e.x, e.y)) press_on(btn_undo);
            else if (hit(btn_level, e.x, e.y)) press_on(btn_level);
            else if (hit(btn_side, e.x, e.y)) press_on(btn_side);
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) { press_forget(); break; }
        if (hit(btn_new, e.x, e.y) || hit(btn_side, e.x, e.y)) {
            press_forget();
            if (hit(btn_side, e.x, e.y)) player = -player;
            start_new();
            draw_page();
            flip();
            if (side != player) engine_move();
            continue;
        }
        if (hit(btn_level, e.x, e.y)) {
            press_forget();
            lv = (lv + 1) % 3;
            save_game();
            draw_buttons();
            refresh(0, BUTTONS_Y - 10, W, BUTTONS_H + 20, WF_GC16, 0, 0);
            continue;
        }
        if (hit(btn_undo, e.x, e.y)) {
            press_forget();
            /* take back the engine's reply too, so it's your move again */
            if (nhist) unmake();
            while (nhist && side != player) unmake();
            selected = 0;
            last_from = nhist ? hist[nhist - 1].m.from : 0;
            last_to = nhist ? hist[nhist - 1].m.to : 0;
            save_game();
            set_turn_status(0);
            draw_board();
            draw_status();
            draw_buttons();
            refresh(0, BY - 4, W, H - BY + 4, WF_GC16, 0, 0);
            continue;
        }
        int sq = sq_at(e.x, e.y);
        if (!sq || over || side != player) continue;
        move l[256];
        int n = legal_moves(l), done = 0;
        if (selected) {
            /* a promotion becomes a queen */
            for (int i = 0; i < n && !done; i++)
                if (l[i].from == selected && l[i].to == sq && (!l[i].promo || l[i].promo == Q)) {
                    char s[16], msg[64];
                    san(l[i], s);
                    make(l[i]);
                    last_from = l[i].from; last_to = l[i].to;
                    selected = 0;
                    snprintf(msg, sizeof msg, "You played %s.", s);
                    set_turn_status(msg);
                    save_game();
                    draw_board();
                    draw_status();
                    refresh(0, BY - 4, W, STATUS_Y + 64 - BY + 4, WF_GC16, 0, 0);
                    done = 1;
                }
            if (done) {
                if (!over) engine_move();
                continue;
            }
        }
        int own = bd[sq] != EMPTY && (bd[sq] > 0) == (player > 0);
        selected = own && sq != selected ? sq : 0;
        draw_board();
        refresh_board();
    }
    save_game();
    return 0;
}

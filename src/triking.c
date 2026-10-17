/* TriKing on a Kindle Paperwhite 3. A port of ~/tri-king/main.gd (Godot 4)
 * to the freestanding Kindle layer in kio.h. The rules follow the original
 * line for line: an 8x18 board, six three-cell pieces from an eight-piece
 * bag, rotation with a one-column kick, Tri-Spins, combos and the
 * max(0.1, 1.0 - (L - 1) * 0.1) fall interval.
 *
 * E-ink has no colour, so each of the six pieces gets its own black and
 * white fill pattern, which also lets every refresh use the fast DU
 * waveform. Gestures match the original: tap the board to rotate, drag to
 * move, drag down to soft drop, flick down to hard drop. A button row is
 * added below the board.
 */
#include "kio.h"

#define COLS 8
#define ROWS 18
#define CELL 64

static const s32 SHAPES[6][3][2] = {
    {{0, -1}, {0, 0}, {0, 1}},
    {{0, -1}, {0, 0}, {1, 0}},
    {{-1, -1}, {0, 0}, {1, 1}},
    {{-1, -1}, {0, 0}, {1, -1}},
    {{0, -1}, {0, 0}, {-1, 1}},
    {{0, -1}, {0, 0}, {1, 1}},
};

enum { MENU, PLAYING, PAUSED, GAMEOVER };

static s32 board[ROWS][COLS]; /* 0 empty, else piece index + 1 */
static s32 piece[3][2];
static int piece_kind, px_, py_;
static int state = MENU;
static int queue[3], nq;
static int bag[8], nbag;
static int score, lines, level = 1, combo = -1;
static int last_rotate;
static char banner[32];
static double fall_interval = 1.0, next_fall;
static int soft_drop;

/* ---- randomness ------------------------------------------------------- */

static u32 rng = 2463534242u;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int next_from_bag(void) {
    if (nbag == 0) {
        static const int fill_[8] = {0, 0, 1, 1, 2, 3, 4, 5};
        for (int i = 0; i < 8; i++) bag[i] = fill_[i];
        for (int i = 7; i > 0; i--) { int j = (int)(rnd() % (u32)(i + 1)), t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
        nbag = 8;
    }
    return bag[--nbag];
}
static int queue_pop(void) { int v = queue[0]; queue[0] = queue[1]; queue[1] = queue[2]; nq--; return v; }
static void queue_push(int v) { queue[nq++] = v; }

/* ---- rules, as in main.gd --------------------------------------------- */

static int valid(int dx, int dy) {
    for (int i = 0; i < 3; i++) {
        int x = px_ + piece[i][0] + dx, y = py_ + piece[i][1] + dy;
        if (x < 0 || x >= COLS || y >= ROWS || (y >= 0 && board[y][x])) return 0;
    }
    return 1;
}

static double base_interval(void) {
    double t = 1.0 - (level - 1) * 0.1;
    return t < 0.1 ? 0.1 : t;
}

static void spawn(int first) {
    int k;
    if (first) { k = (int)(rnd() & 1); queue_pop(); }
    else k = queue_pop();
    queue_push(next_from_bag());
    piece_kind = k;
    for (int i = 0; i < 3; i++) piece[i][0] = SHAPES[k][i][0], piece[i][1] = SHAPES[k][i][1];
    px_ = COLS / 2; py_ = 1;
    last_rotate = 0;
}

static void game_over(void) { state = GAMEOVER; }

static void check_lines(int spin) {
    int cleared = 0;
    for (int y = 0; y < ROWS; y++) {
        int full = 1;
        for (int x = 0; x < COLS; x++) if (!board[y][x]) { full = 0; break; }
        if (!full) continue;
        cleared++;
        for (int yy = y; yy > 0; yy--) for (int x = 0; x < COLS; x++) board[yy][x] = board[yy - 1][x];
        for (int x = 0; x < COLS; x++) board[0][x] = 0;
    }
    if (cleared) {
        combo++;
        static const int base[4] = {0, 100, 300, 500};
        int pts = base[cleared > 3 ? 0 : cleared] * level;
        if (spin) {
            pts *= 4;
            char *b = banner; const char *a = "TRI-SPIN! ";
            while (*a) *b++ = *a++;
            *b++ = (char)('0' + cleared);
            a = cleared == 1 ? " LINE!" : " LINES!";
            while (*a) *b++ = *a++;
            *b = 0;
        } else if (combo > 0) {
            int n = combo, i = 0; char d[8];
            do { d[i++] = (char)('0' + n % 10); n /= 10; } while (n);
            char *b = banner; while (i) *b++ = d[--i];
            const char *a = " COMBO!"; while (*a) *b++ = *a++;
            *b = 0;
        } else banner[0] = 0;
        score += pts + 50 * combo * level;
        lines += cleared;
        level = lines / 10 + 1;
        fall_interval = base_interval();
    } else {
        combo = -1;
        banner[0] = 0;
    }
}

static void lock_piece(void) {
    int spin = last_rotate && !valid(0, -1) && !valid(-1, 0) && !valid(1, 0);
    for (int i = 0; i < 3; i++) {
        int x = px_ + piece[i][0], y = py_ + piece[i][1];
        if (y < 0) { game_over(); return; }
        board[y][x] = piece_kind + 1;
    }
    check_lines(spin);
    if (state == PLAYING) {
        spawn(0);
        if (!valid(0, 0)) game_over();
    }
}

static void rotate(void) {
    s32 old[3][2];
    for (int i = 0; i < 3; i++) {
        old[i][0] = piece[i][0]; old[i][1] = piece[i][1];
        s32 x = piece[i][0]; piece[i][0] = -piece[i][1]; piece[i][1] = x;
    }
    if (!valid(0, 0)) {
        if (valid(1, 0)) px_++;
        else if (valid(-1, 0)) px_--;
        else { for (int i = 0; i < 3; i++) piece[i][0] = old[i][0], piece[i][1] = old[i][1]; return; }
    }
    last_rotate = 1;
}

static void move(int dx) { if (valid(dx, 0)) { px_ += dx; last_rotate = 0; } }

static void soft_step(void) {
    if (valid(0, 1)) { py_++; score++; last_rotate = 0; next_fall = now() + fall_interval; }
}

static void hard_drop(void) {
    int d = 0;
    while (valid(0, 1)) { py_++; d += 2; }
    score += d;
    lock_piece();
    next_fall = now() + fall_interval;
}

static void reset_game(void) {
    for (int y = 0; y < ROWS; y++) for (int x = 0; x < COLS; x++) board[y][x] = 0;
    score = 0; lines = 0; level = 1; combo = -1; banner[0] = 0;
    nbag = 0; nq = 0;
    for (int i = 0; i < 3; i++) queue_push(next_from_bag());
    fall_interval = 1.0;
    state = PLAYING;
    spawn(1);
    next_fall = now() + fall_interval;
}

static int ghost_y(void) {
    int g = py_;
    for (;;) {
        for (int i = 0; i < 3; i++) {
            int x = px_ + piece[i][0], y = g + piece[i][1] + 1;
            if (y >= ROWS || (y >= 0 && board[y][x])) return g;
        }
        g++;
    }
}

/* ---- drawing ---------------------------------------------------------- */

static int BX, BY; /* board origin */
enum { K_LEFT, K_ROT, K_RIGHT, K_DOWN, K_DROP, K_PAUSE, K_QUIT, NK };
static struct { int x, y, w, h; const char *label; } keys[NK];
static struct { int x, y, w, h; } start_btn, resume_btn, restart_btn, quit_btn;

/* One pattern per piece, pure black and white so DU can draw it. */
static int pattern(int kind, int x, int y) {
    switch (kind) {
    case 0: return 1;                                   /* solid */
    case 1: return ((x + y) / 6) % 2;                   /* diagonal stripes */
    case 2: return ((x / 8) + (y / 8)) % 2;             /* checker */
    case 3: return (y / 6) % 2;                         /* horizontal stripes */
    case 4: return (x / 6) % 2;                         /* vertical stripes */
    default: { int cx = x % 12 - 6, cy = y % 12 - 6; return cx * cx + cy * cy < 13; } /* dots */
    }
}

static void draw_block(int x, int y, int size, int kind) {
    fill(x, y, size, size, 0xFF);
    int in = size / 10;
    for (int j = in; j < size - in; j++)
        for (int i = in; i < size - in; i++)
            if (pattern(kind, i, j)) px(x + i, y + j, 0x00);
    frame_rect(x + 1, y + 1, size - 2, size - 2, size >= 48 ? 4 : 2, 0x00);
}

static void draw_ghost(int x, int y) {
    for (int i = 6; i < CELL - 6; i++)
        if ((i / 6) % 2 == 0) {
            px(x + i, y + 4, 0); px(x + i, y + 5, 0);
            px(x + i, y + CELL - 5, 0); px(x + i, y + CELL - 6, 0);
            px(x + 4, y + i, 0); px(x + 5, y + i, 0);
            px(x + CELL - 5, y + i, 0); px(x + CELL - 6, y + i, 0);
        }
}

static void num(char *out, int v) {
    char d[12]; int i = 0;
    do { d[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) *out++ = d[--i];
    *out = 0;
}

static void centered(int cx, int y, int s, const char *t, u8 g) { text(cx - text_w(t, s) / 2, y, s, t, g); }

static void button(int x, int y, int w, int h, const char *l, int s) {
    fill(x, y, w, h, 0xFF);
    frame_rect(x, y, w, h, 5, 0x00);
    while (s > 2 && text_w(l, s) > w - 20) s--;
    text(x + (w - text_w(l, s)) / 2, y + (h - 7 * s) / 2, s, l, 0x00);
}

static void layout(void) {
    BX = 48; BY = 110;
    int ky = BY + ROWS * CELL + 24, kh = H - ky - 16, kw = (W - 16 * 2 - 8 * 6) / 7;
    static const char *names[NK] = {"LEFT", "ROT", "RIGHT", "DOWN", "DROP", "PAUSE", "QUIT"};
    for (int i = 0; i < NK; i++) {
        keys[i].x = 16 + i * (kw + 8); keys[i].y = ky; keys[i].w = kw; keys[i].h = kh; keys[i].label = names[i];
    }
    start_btn.w = resume_btn.w = restart_btn.w = quit_btn.w = 480;
    start_btn.h = resume_btn.h = restart_btn.h = quit_btn.h = 120;
    start_btn.x = resume_btn.x = restart_btn.x = quit_btn.x = (W - 480) / 2;
    start_btn.y = 760; resume_btn.y = 700; restart_btn.y = 860; quit_btn.y = 1020;
}

static void draw_overlay(const char *title, int sub_score) {
    int x = 120, y = 300, w = W - 240, h = 900;
    fill(x, y, w, h, 0xFF);
    frame_rect(x, y, w, h, 8, 0x00);
    centered(W / 2, y + 80, 14, title, 0x00);
    if (sub_score >= 0) {
        char b[16]; num(b, sub_score);
        centered(W / 2, y + 260, 6, "SCORE", 0x00);
        centered(W / 2, y + 330, 8, b, 0x00);
    }
}

static void draw(void) {
    fill(0, 0, W, H, 0xFF);
    text(BX, 30, 9, "TRIKING", 0x00);
    if (banner[0]) text(BX + 540, 50, 4, banner, 0x00);

    /* board */
    frame_rect(BX - 8, BY - 8, COLS * CELL + 16, ROWS * CELL + 16, 4, 0x00);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            int cx = BX + x * CELL, cy = BY + y * CELL;
            if (board[y][x]) draw_block(cx, cy, CELL, board[y][x] - 1);
            else px(cx + CELL / 2, cy + CELL / 2, 0x00), px(cx + CELL / 2 + 1, cy + CELL / 2, 0x00);
        }
    if (state == PLAYING || state == PAUSED) {
        int g = ghost_y();
        for (int i = 0; i < 3; i++) {
            int y = g + piece[i][1];
            if (y >= 0 && g != py_) draw_ghost(BX + (px_ + piece[i][0]) * CELL, BY + y * CELL);
        }
        for (int i = 0; i < 3; i++) {
            int y = py_ + piece[i][1];
            if (y >= 0) draw_block(BX + (px_ + piece[i][0]) * CELL, BY + y * CELL, CELL, piece_kind);
        }
    }

    /* side panel */
    int sx = BX + COLS * CELL + 60;
    char b[16];
    text(sx, BY, 5, "NEXT", 0x00);
    for (int i = 0; i < nq; i++)
        for (int k = 0; k < 3; k++)
            draw_block(sx + 90 + SHAPES[queue[i]][k][0] * 40, BY + 130 + i * 150 + SHAPES[queue[i]][k][1] * 40, 40, queue[i]);
    text(sx, BY + 600, 5, "SCORE", 0x00); num(b, score); text(sx, BY + 660, 7, b, 0x00);
    text(sx, BY + 780, 5, "LEVEL", 0x00); num(b, level); text(sx, BY + 840, 7, b, 0x00);
    text(sx, BY + 960, 5, "LINES", 0x00); num(b, lines); text(sx, BY + 1020, 7, b, 0x00);

    for (int i = 0; i < NK; i++) {
        const char *l = keys[i].label;
        if (i == K_PAUSE && state == PAUSED) l = "PLAY";
        button(keys[i].x, keys[i].y, keys[i].w, keys[i].h, l, 5);
    }

    if (state == MENU) {
        draw_overlay("TRIKING", -1);
        centered(W / 2, 560, 4, "TAP TO ROTATE  DRAG TO MOVE", 0x00);
        centered(W / 2, 620, 4, "FLICK DOWN TO DROP", 0x00);
        button(start_btn.x, start_btn.y, start_btn.w, start_btn.h, "START GAME", 7);
        button(quit_btn.x, quit_btn.y, quit_btn.w, quit_btn.h, "QUIT", 7);
    } else if (state == PAUSED) {
        draw_overlay("PAUSED", score);
        button(resume_btn.x, resume_btn.y, resume_btn.w, resume_btn.h, "RESUME", 7);
        button(restart_btn.x, restart_btn.y, restart_btn.w, restart_btn.h, "RESTART", 7);
        button(quit_btn.x, quit_btn.y, quit_btn.w, quit_btn.h, "QUIT", 7);
    } else if (state == GAMEOVER) {
        draw_overlay("GAME OVER", score);
        button(restart_btn.x, restart_btn.y, restart_btn.w, restart_btn.h, "PLAY AGAIN", 7);
        button(quit_btn.x, quit_btn.y, quit_btn.w, quit_btn.h, "QUIT", 7);
    }
}

#define IN(r, X, Y) ((X) >= (r).x && (X) < (r).x + (r).w && (Y) >= (r).y && (Y) < (r).y + (r).h)

/* ---- main ------------------------------------------------------------- */

static int app_main(void) {
    if (fb_init() < 0) return 1;
    layout();
    open_input();
    if (touchfd < 0) logs("no touchscreen found\n");
    rng ^= (u32)(now() * 1000003.0);
    for (int i = 0; i < 3; i++) queue_push(next_from_bag());

    int dirty = 1, frames = 0;
    /* the one finger we follow for gestures */
    int tracking = -1, sx = 0, sy = 0, dragged = 0, accx = 0, accy = 0, lastx = 0, lasty = 0;
    double t0 = 0;
    const int DRAG = 64, SWIPE = 110;

    draw();
    refresh(0, 0, W, H, WF_GC16, 1, 1);
    dirty = 0;

    for (;;) {
        if (poll_input()) break;
        double t = now();
        int quit = 0;

        for (int i = 0; i < NSLOT; i++) {
            if (slot[i].id >= 0 && slot[i].fresh) {
                slot[i].fresh = 0;
                int x = slot[i].x, y = slot[i].y;
                if (state == MENU) {
                    if (IN(start_btn, x, y)) reset_game(), dirty = 1;
                    else if (IN(quit_btn, x, y)) quit = 1;
                } else if (state == PAUSED) {
                    if (IN(resume_btn, x, y)) state = PLAYING, next_fall = t + fall_interval, dirty = 1;
                    else if (IN(restart_btn, x, y)) reset_game(), dirty = 1;
                    else if (IN(quit_btn, x, y)) quit = 1;
                } else if (state == GAMEOVER) {
                    if (IN(restart_btn, x, y)) reset_game(), dirty = 1;
                    else if (IN(quit_btn, x, y)) quit = 1;
                } else {
                    int k = -1;
                    for (int j = 0; j < NK; j++) if (IN(keys[j], x, y)) k = j;
                    if (k == K_LEFT) move(-1);
                    else if (k == K_RIGHT) move(1);
                    else if (k == K_ROT) rotate();
                    else if (k == K_DOWN) soft_step();
                    else if (k == K_DROP) hard_drop();
                    else if (k == K_PAUSE) state = PAUSED;
                    else if (k == K_QUIT) quit = 1;
                    else if (tracking < 0) {
                        tracking = i; sx = lastx = x; sy = lasty = y; t0 = t;
                        dragged = 0; accx = accy = 0;
                    }
                    dirty = 1;
                }
            }
        }
        if (quit) break;

        /* gestures on the board, as in main.gd's _input */
        if (tracking >= 0 && state == PLAYING) {
            if (slot[tracking].id >= 0) {
                accx += slot[tracking].x - lastx; accy += slot[tracking].y - lasty;
                lastx = slot[tracking].x; lasty = slot[tracking].y;
                while (accx > DRAG) { if (valid(1, 0)) px_++, dragged = 1, last_rotate = 0, dirty = 1; accx -= DRAG; }
                while (accx < -DRAG) { if (valid(-1, 0)) px_--, dragged = 1, last_rotate = 0, dirty = 1; accx += DRAG; }
                if (accy > DRAG) { soft_drop = 1; accy = 0; }
            } else {
                int dx = lastx - sx, dy = lasty - sy;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                soft_drop = 0;
                if (dy > SWIPE && ady > adx && t - t0 < 0.3) hard_drop();
                else if (!dragged && adx + ady < 30) rotate();
                tracking = -1;
                dirty = 1;
            }
        } else if (tracking >= 0) tracking = -1;

        if (state == PLAYING) {
            double iv = soft_drop ? 0.05 : fall_interval;
            if (next_fall - t > iv) next_fall = t + iv;
            if (t >= next_fall) {
                if (valid(0, 1)) py_++;
                else lock_piece();
                next_fall = t + iv;
                dirty = 1;
            }
        }

        if (dirty) {
            draw();
            frames++;
            if (frames % 60 == 0 || state != PLAYING) refresh(0, 0, W, H, WF_GC16, 1, 1);
            else refresh(0, 0, W, H, WF_DU, 0, 1);
            dirty = 0;
        } else nap_ms(10);
    }

    fill(0, 0, W, H, 0xFF);
    refresh(0, 0, W, H, WF_GC16, 1, 1);
    return 0;
}

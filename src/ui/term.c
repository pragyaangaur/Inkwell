/* term: a terminal for Inkwell. It runs a shell in a pseudo-terminal and
 * understands enough of the VT102 control codes for the shell, vi, top and
 * an SSH session to another computer (ssh is Dropbear's client). Text is
 * drawn in pure black and white, without anti-aliasing, so the fast A2
 * refresh shows every keystroke exactly. A row of extra keys (Esc, Tab,
 * Ctrl, arrows and the symbols a shell needs) sits above the keyboard, and
 * the keyboard can be hidden to see the whole screen. */
#include "ui.h"
#include <errno.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

/* ---- the screen grid ------------------------------------------------------- */

#define MAXC 100
#define MAXR 60
#define PX 28
#define TOP_H 110
#define EXTRA_H 90
#define TERM_X 12

enum { A_BOLD = 1, A_REV = 2, A_UNDER = 4 };
typedef struct { int c; unsigned char a; } cell;

static cell grid[MAXR][MAXC], alt_grid[MAXR][MAXC];
static int cols, rows, cw, lh, term_y;
static int cx, cy, wrap_pending, attr;
static int top_m, bot_m; /* scroll region, inclusive */
static int save_cx, save_cy, save_attr;
static int alt_on, alt_cx, alt_cy;
static int cursor_on = 1, app_cursor;
static unsigned char dirty_row[MAXR];
static int show_kb = 1;

static int master = -1;
static pid_t child;
static int exited;

static void dirty_all(void) { memset(dirty_row, 1, sizeof dirty_row); }

static void clear_cells(int r, int c0, int c1) {
    for (int c = c0; c < c1 && c < MAXC; c++) grid[r][c] = (cell){' ', 0};
    dirty_row[r] = 1;
}
static void clear_rows(int r0, int r1) {
    for (int r = r0; r < r1; r++) clear_cells(r, 0, MAXC);
}

static void scroll_up(int t, int b, int n) {
    if (n > b - t + 1) n = b - t + 1;
    memmove(grid[t], grid[t + n], sizeof grid[0] * (b - t + 1 - n));
    clear_rows(b - n + 1, b + 1);
    for (int r = t; r <= b; r++) dirty_row[r] = 1;
}
static void scroll_down(int t, int b, int n) {
    if (n > b - t + 1) n = b - t + 1;
    memmove(grid[t + n], grid[t], sizeof grid[0] * (b - t + 1 - n));
    clear_rows(t, t + n);
    for (int r = t; r <= b; r++) dirty_row[r] = 1;
}

static void clamp_cursor(void) {
    if (cx < 0) cx = 0;
    if (cx >= cols) cx = cols - 1;
    if (cy < 0) cy = 0;
    if (cy >= rows) cy = rows - 1;
}
static void move_to(int r, int c) {
    dirty_row[cy] = 1;
    cy = r; cx = c; wrap_pending = 0;
    clamp_cursor();
    dirty_row[cy] = 1;
}

static void line_feed(void) {
    dirty_row[cy] = 1;
    if (cy == bot_m) scroll_up(top_m, bot_m, 1);
    else if (cy < rows - 1) cy++;
    dirty_row[cy] = 1;
}

static void put_char(int c) {
    if (wrap_pending) { cx = 0; line_feed(); wrap_pending = 0; }
    grid[cy][cx] = (cell){c, (unsigned char)attr};
    dirty_row[cy] = 1;
    if (cx == cols - 1) wrap_pending = 1;
    else cx++;
}

static void reset_term(void) {
    attr = 0; top_m = 0; bot_m = rows - 1; cursor_on = 1; app_cursor = 0;
    clear_rows(0, MAXR);
    move_to(0, 0);
}

/* ---- the control code parser ----------------------------------------------- */

enum { S_TEXT, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_SKIP };
static int state, nparam, param[16], priv;
static int utf_left, utf_cp;

static void reply(const char *s) { if (master >= 0) write(master, s, strlen(s)); }

static int P(int i, int def) { return i < nparam && param[i] > 0 ? param[i] : def; }

static void set_mode(int on) {
    for (int i = 0; i < nparam || i == 0; i++) {
        int m = i < nparam ? param[i] : 0;
        if (!priv) continue;
        if (m == 1) app_cursor = on;
        else if (m == 25) { cursor_on = on; dirty_row[cy] = 1; }
        else if (m == 47 || m == 1047 || m == 1049) {
            if (on == alt_on) continue;
            cell tmp[MAXR][MAXC];
            memcpy(tmp, grid, sizeof grid);
            memcpy(grid, alt_grid, sizeof grid);
            memcpy(alt_grid, tmp, sizeof grid);
            if (on) { alt_cx = cx; alt_cy = cy; clear_rows(0, MAXR); }
            else { cx = alt_cx; cy = alt_cy; clamp_cursor(); }
            alt_on = on;
            dirty_all();
        }
    }
}

static void sgr(void) {
    if (!nparam) { attr = 0; return; }
    for (int i = 0; i < nparam; i++) {
        int p = param[i];
        if (p == 0) attr = 0;
        else if (p == 1) attr |= A_BOLD;
        else if (p == 4) attr |= A_UNDER;
        else if (p == 7) attr |= A_REV;
        else if (p == 22) attr &= ~A_BOLD;
        else if (p == 24) attr &= ~A_UNDER;
        else if (p == 27) attr &= ~A_REV;
        else if (p == 38 || p == 48) { /* colours have no meaning here: skip their arguments */
            if (i + 1 < nparam && param[i + 1] == 5) i += 2;
            else if (i + 1 < nparam && param[i + 1] == 2) i += 4;
        }
    }
}

static void csi(int f) {
    int n = P(0, 1);
    switch (f) {
    case 'A': move_to(cy - n, cx); break;
    case 'B': case 'e': move_to(cy + n, cx); break;
    case 'C': case 'a': move_to(cy, cx + n); break;
    case 'D': move_to(cy, cx - n); break;
    case 'E': move_to(cy + n, 0); break;
    case 'F': move_to(cy - n, 0); break;
    case 'G': case '`': move_to(cy, n - 1); break;
    case 'd': move_to(n - 1, cx); break;
    case 'H': case 'f': move_to(P(0, 1) - 1, P(1, 1) - 1); break;
    case 'J': {
        int m = nparam ? param[0] : 0;
        if (m == 0) { clear_cells(cy, cx, cols); clear_rows(cy + 1, rows); }
        else if (m == 1) { clear_rows(0, cy); clear_cells(cy, 0, cx + 1); }
        else clear_rows(0, rows);
        break;
    }
    case 'K': {
        int m = nparam ? param[0] : 0;
        if (m == 0) clear_cells(cy, cx, cols);
        else if (m == 1) clear_cells(cy, 0, cx + 1);
        else clear_cells(cy, 0, cols);
        break;
    }
    case 'L': if (cy >= top_m && cy <= bot_m) scroll_down(cy, bot_m, n); break;
    case 'M': if (cy >= top_m && cy <= bot_m) scroll_up(cy, bot_m, n); break;
    case 'S': scroll_up(top_m, bot_m, n); break;
    case 'T': scroll_down(top_m, bot_m, n); break;
    case 'P': {
        if (n > cols - cx) n = cols - cx;
        memmove(&grid[cy][cx], &grid[cy][cx + n], sizeof(cell) * (cols - cx - n));
        clear_cells(cy, cols - n, cols);
        break;
    }
    case '@': {
        if (n > cols - cx) n = cols - cx;
        memmove(&grid[cy][cx + n], &grid[cy][cx], sizeof(cell) * (cols - cx - n));
        clear_cells(cy, cx, cx + n);
        break;
    }
    case 'X': clear_cells(cy, cx, cx + n); break;
    case 'm': sgr(); break;
    case 'r':
        top_m = P(0, 1) - 1; bot_m = P(1, rows) - 1;
        if (top_m < 0 || bot_m >= rows || top_m >= bot_m) { top_m = 0; bot_m = rows - 1; }
        move_to(0, 0);
        break;
    case 's': save_cx = cx; save_cy = cy; break;
    case 'u': move_to(save_cy, save_cx); break;
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 'n':
        if (P(0, 0) == 6) { char b[32]; snprintf(b, sizeof b, "\033[%d;%dR", cy + 1, cx + 1); reply(b); }
        else if (P(0, 0) == 5) reply("\033[0n");
        break;
    case 'c': if (!priv) reply("\033[?6c"); break;
    }
}

static void feed(unsigned char ch) {
    switch (state) {
    case S_TEXT:
        if (utf_left) {
            if ((ch & 0xC0) == 0x80) {
                utf_cp = (utf_cp << 6) | (ch & 63);
                if (--utf_left == 0) put_char(utf_cp);
                return;
            }
            utf_left = 0;
        }
        if (ch == 27) { state = S_ESC; return; }
        if (ch == '\r') { dirty_row[cy] = 1; cx = 0; wrap_pending = 0; return; }
        if (ch == '\n' || ch == 11 || ch == 12) { line_feed(); wrap_pending = 0; return; }
        if (ch == 8) { if (cx > 0) cx--; wrap_pending = 0; dirty_row[cy] = 1; return; }
        if (ch == '\t') { int t = (cx / 8 + 1) * 8; move_to(cy, t < cols ? t : cols - 1); return; }
        if (ch < 32 || ch == 127) return;
        if (ch >= 0xF0) { utf_cp = ch & 7; utf_left = 3; return; }
        if (ch >= 0xE0) { utf_cp = ch & 15; utf_left = 2; return; }
        if (ch >= 0xC0) { utf_cp = ch & 31; utf_left = 1; return; }
        if (ch >= 0x80) return;
        put_char(ch);
        return;
    case S_ESC:
        state = S_TEXT;
        if (ch == '[') { state = S_CSI; nparam = 0; priv = 0; memset(param, 0, sizeof param); }
        else if (ch == ']') state = S_OSC;
        else if (ch == '(' || ch == ')' || ch == '#' || ch == '%') state = S_SKIP;
        else if (ch == '7') { save_cx = cx; save_cy = cy; save_attr = attr; }
        else if (ch == '8') { move_to(save_cy, save_cx); attr = save_attr; }
        else if (ch == 'D') line_feed();
        else if (ch == 'E') { cx = 0; line_feed(); }
        else if (ch == 'M') { if (cy == top_m) scroll_down(top_m, bot_m, 1); else move_to(cy - 1, cx); }
        else if (ch == 'c') reset_term();
        return;
    case S_CSI:
        if (ch >= '0' && ch <= '9') {
            if (!nparam) nparam = 1;
            param[nparam - 1] = param[nparam - 1] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (!nparam) nparam = 1;
            if (nparam < 16) nparam++;
        } else if (ch == '?' || ch == '>' || ch == '=') {
            priv = 1;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            csi(ch);
            state = S_TEXT;
        } else if (ch < 32) {
            /* control characters still act in the middle of a sequence */
            int s = state; state = S_TEXT; feed(ch); if (state == S_TEXT) state = s;
        }
        return;
    case S_OSC: /* window titles and the like: read to the end and drop */
        if (ch == 7) state = S_TEXT;
        else if (ch == 27) state = S_OSC_ESC;
        return;
    case S_OSC_ESC:
        state = ch == '\\' ? S_TEXT : S_OSC;
        return;
    case S_SKIP:
        state = S_TEXT;
        return;
    }
}

/* ---- drawing ----------------------------------------------------------------- */

/* One glyph in pure black or white: alpha above half becomes ink. */
static void put_glyph(int c, int x, int y, unsigned char ink, int bold) {
    const glyph *g = glyph_get(&f_mono, PX, c);
    int base = y + font_ascent(&f_mono, PX);
    for (int pass = 0; pass <= bold; pass++)
        for (int j = 0; j < g->h; j++) {
            int yy = base + g->oy + j;
            if (yy < 0 || yy >= H) continue;
            const unsigned char *src = g->bm + j * g->w;
            unsigned char *row = fb + yy * PITCH;
            for (int i = 0; i < g->w; i++) {
                int xx = x + g->ox + i + pass;
                if (src[i] > 110 && xx >= 0 && xx < W) row[xx] = ink;
            }
        }
}

static void draw_row(int r) {
    int y = term_y + r * lh;
    fill(0, y, W, lh, WHITE);
    for (int c = 0; c < cols; c++) {
        cell k = grid[r][c];
        int rev = (k.a & A_REV) != 0;
        if (cursor_on && !exited && r == cy && c == cx) rev = !rev;
        int x = TERM_X + c * cw;
        if (rev) fill(x, y, cw, lh, BLACK);
        if (k.c > ' ') put_glyph(k.c, x, y + (lh - PX) / 2 - 2, rev ? WHITE : BLACK, k.a & A_BOLD);
        if (k.a & A_UNDER) fill(x, y + lh - 4, cw, 2, rev ? WHITE : BLACK);
    }
}

/* ---- the extra keys and the top bar ------------------------------------------ */

enum { X_ESC = 1000, X_TAB, X_CTRL, X_LEFT, X_DOWN, X_UP, X_RIGHT };
static const struct { int code; const char *label; } extra_def[] = {
    {X_ESC, "esc"}, {X_TAB, "tab"}, {X_CTRL, "ctrl"}, {'~', "~"}, {'`', "`"}, {'<', "<"}, {'>', ">"},
    {X_LEFT, "\xE2\x86\x90"}, {X_DOWN, "\xE2\x86\x93"}, {X_UP, "\xE2\x86\x91"}, {X_RIGHT, "\xE2\x86\x92"},
};
#define NEXTRA (int)(sizeof extra_def / sizeof extra_def[0])
static rect extra_r[NEXTRA];
static int ctrl_on;
static const rect back_r = {0, 0, 160, TOP_H};
static rect kb_btn;

static int extra_y(void) { return KB_TOP - 22 - EXTRA_H; }

static void draw_extra(void) {
    int y = extra_y(), gap = 8, kw = (W - 44 - (NEXTRA - 1) * gap) / NEXTRA;
    fill(0, y, W, EXTRA_H, 0xE8);
    for (int i = 0; i < NEXTRA; i++) {
        rect r = {22 + i * (kw + gap), y + 10, kw, EXTRA_H - 16};
        extra_r[i] = r;
        int on = extra_def[i].code == X_CTRL && ctrl_on;
        rrect_fill(r.x, r.y + 4, r.w, r.h, 12, 0xA8);
        rrect_fill(r.x, r.y, r.w, r.h, 12, on ? BLACK : WHITE);
        int px = strlen(extra_def[i].label) > 1 && extra_def[i].code < X_LEFT ? 28 : 40;
        text_center(&f_ui_bold, px, r.x + r.w / 2, r.y + (r.h - px) / 2 - 4, extra_def[i].label, on ? WHITE : BLACK);
    }
}

static void draw_top(void) {
    fill(0, 0, W, TOP_H, WHITE);
    float ccx = 68, ccy = TOP_H / 2.0f;
    ring(ccx, ccy, 30, 3, BLACK);
    line(ccx + 7, ccy - 13, ccx - 7, ccy, 5, BLACK);
    line(ccx - 7, ccy, ccx + 7, ccy + 13, 5, BLACK);
    text(&f_serif_bold, 60, 120, 18, "Terminal", BLACK);
    kb_btn = (rect){W - MARGIN - 230, 22, 230, 66};
    pill(kb_btn, show_kb ? "Hide keys" : "Keys", 0);
    fill(TERM_X, TOP_H - 2, W - 2 * TERM_X, 2, BLACK);
}

/* Works out the grid size for the space left by the keyboard, and tells
 * the shell when it changes. */
static void set_geometry(void) {
    cw = glyph_get(&f_mono, PX, 'M')->adv;
    lh = PX + 6;
    term_y = TOP_H + 8;
    int bottom = show_kb ? extra_y() - 6 : H - 8;
    int nc = (W - 2 * TERM_X) / cw, nr = (bottom - term_y) / lh;
    if (nc > MAXC) nc = MAXC;
    if (nr > MAXR) nr = MAXR;
    if (rows && nr < rows && cy >= nr) { /* keep the cursor line in view */
        int up = cy - nr + 1;
        memmove(grid[0], grid[up], sizeof grid[0] * (MAXR - up));
        for (int r = MAXR - up; r < MAXR; r++) for (int c = 0; c < MAXC; c++) grid[r][c] = (cell){' ', 0};
        cy -= up;
    }
    cols = nc; rows = nr;
    top_m = 0; bot_m = rows - 1;
    clamp_cursor();
    if (master >= 0) {
        struct winsize ws = {(unsigned short)rows, (unsigned short)cols, 0, 0};
        ioctl(master, TIOCSWINSZ, &ws);
    }
    dirty_all();
}

static void draw_all(void) {
    fill(0, 0, W, H, WHITE);
    draw_top();
    for (int r = 0; r < rows; r++) draw_row(r);
    if (show_kb) { draw_extra(); kb_draw(); }
    memset(dirty_row, 0, sizeof dirty_row);
}

/* ---- the shell ------------------------------------------------------------------ */

static void start_shell(void) {
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, 0, 0};
    child = forkpty(&master, 0, 0, &ws);
    if (child == 0) {
        setenv("TERM", "vt102", 1);
        setenv("HOME", "/root", 1);
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("PS1", "\\w # ", 1);
        if (chdir("/root")) {}
        execl("/bin/sh", "-sh", (char *)0);
        _exit(127);
    }
    if (child < 0) { master = -1; return; }
    fcntl(master, F_SETFL, O_NONBLOCK);
    exited = 0;
}

static void say(const char *s) { while (*s) feed((unsigned char)*s++); }

static void send_key(int c) {
    if (exited) {
        if (c == K_ENTER) { reset_term(); start_shell(); }
        return;
    }
    char b[8];
    int n = 0;
    const char *seq = 0;
    if (c == K_ENTER) b[n++] = '\r';
    else if (c == K_BACK) b[n++] = 127;
    else if (c == X_ESC) b[n++] = 27;
    else if (c == X_TAB) b[n++] = '\t';
    else if (c == X_UP) seq = app_cursor ? "\033OA" : "\033[A";
    else if (c == X_DOWN) seq = app_cursor ? "\033OB" : "\033[B";
    else if (c == X_RIGHT) seq = app_cursor ? "\033OC" : "\033[C";
    else if (c == X_LEFT) seq = app_cursor ? "\033OD" : "\033[D";
    else if (c > 0 && c < 128) {
        if (ctrl_on) {
            if (c >= 'a' && c <= 'z') c -= 96;
            else if (c >= '@' && c <= '_') c -= 64;
            else if (c == ' ') c = 0;
        }
        b[n++] = (char)c;
    }
    if (seq) write(master, seq, strlen(seq));
    else if (n) write(master, b, n);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Draws the rows that changed and sends them to the panel on A2. */
static int a2_count;
static void flush(void) {
    int r0 = -1, r1 = -1;
    for (int r = 0; r < rows; r++)
        if (dirty_row[r]) { draw_row(r); if (r0 < 0) r0 = r; r1 = r; dirty_row[r] = 0; }
    if (r0 >= 0) {
        refresh(0, term_y + r0 * lh, W, (r1 - r0 + 1) * lh, WF_A2, 0, 0);
        a2_count++;
    }
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    signal(SIGPIPE, SIG_IGN);
    kb_enter_label = "enter";
    set_geometry();
    reset_term();
    say("\033[1mInkwell terminal\033[0m\r\n"
        "ssh you@192.168.15.201 reaches the computer on the USB cable.\r\n\r\n");
    start_shell();
    draw_all();
    flip_flash();

    double dirty_since = 0, last_clean = now_s();
    unsigned char buf[4096];
    for (;;) {
        int pending = 0;
        for (int r = 0; r < rows; r++) pending |= dirty_row[r];
        int timeout = pending ? 15 : a2_count ? 2500 : -1;
        if (evq_head == evq_tail) {
            struct pollfd p[3] = {{touchfd, POLLIN, 0}, {keyfd, POLLIN, 0}, {master, POLLIN, 0}};
            int got = poll(p, 3, timeout), out = 0;
            if (got > 0 && (p[0].revents || p[1].revents)) input_read();
            if (got > 0 && p[2].revents) {
                ssize_t n = read(master, buf, sizeof buf);
                if (n > 0) {
                    for (ssize_t i = 0; i < n; i++) feed(buf[i]);
                    out = 1;
                    if (!dirty_since) dirty_since = now_s();
                } else if (n == 0 || errno != EAGAIN) {
                    close(master);
                    master = -1;
                    waitpid(child, 0, 0);
                    exited = 1;
                    say("\r\n[the shell has exited, press enter for a new one]\r\n");
                }
            }
            /* Output often comes in bursts: wait for a quiet moment before
             * drawing, but draw at least every 120 ms while it streams. */
            pending = 0;
            for (int r = 0; r < rows; r++) pending |= dirty_row[r];
            if (pending && (!out || now_s() - dirty_since > 0.12)) { flush(); dirty_since = 0; }
            if (got == 0 && !pending && a2_count && now_s() - last_clean > 2) {
                /* A calm GC16 pass clears the ghosting A2 leaves behind. */
                refresh(0, term_y, W, rows * lh, WF_GC16, 0, 0);
                a2_count = 0;
                last_clean = now_s();
            }
        }
        event e = input_wait(0);
        if (e.type == EV_NONE) continue;
        if (e.type == EV_POWER) break;
        if (e.type == EV_DOWN) {
            if (hit(back_r, e.x, e.y)) press_on((rect){18, 18, 100, 76});
            else if (hit(kb_btn, e.x, e.y)) press_on(kb_btn);
            else if (show_kb) {
                for (int i = 0; i < NEXTRA; i++) if (hit(extra_r[i], e.x, e.y)) press_on(extra_r[i]);
                kb_down(e.x, e.y);
            }
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_r, e.x, e.y)) { press_forget(); break; }
        if (hit(kb_btn, e.x, e.y) || (!show_kb && e.y > term_y)) {
            press_forget();
            show_kb = !show_kb;
            set_geometry();
            draw_all();
            refresh(0, 0, W, H, WF_GC16, 0, 0);
            continue;
        }
        if (!show_kb) continue;
        int code = K_NONE;
        for (int i = 0; i < NEXTRA; i++)
            if (hit(extra_r[i], e.x, e.y)) code = extra_def[i].code;
        if (code == X_CTRL) {
            press_forget();
            ctrl_on = !ctrl_on;
            draw_extra();
            refresh(0, extra_y(), W, EXTRA_H, WF_GL16, 0, 0);
            continue;
        }
        if (code != K_NONE) press_off();
        else code = kb_tap(e.x, e.y);
        if (code == K_NONE) continue;
        send_key(code);
        if (ctrl_on && code != X_CTRL) {
            ctrl_on = 0;
            draw_extra();
            refresh(0, extra_y(), W, EXTRA_H, WF_GL16, 0, 0);
        }
        if (exited) flush();
    }
    if (child > 0 && !exited) { kill(child, SIGHUP); waitpid(child, 0, 0); }
    return 0;
}

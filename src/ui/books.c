/* books: Inkwell's e-book reader. Pages come from MuPDF through
 * render.js, a helper that keeps the document open under `mutool run`.
 * EPUB, MOBI, FB2 and text reflow to the screen with eight font sizes. PDF
 * pages are shown whole with their margins trimmed, or in Columns mode,
 * which cuts a two-column paper into strips the width of the screen.
 * Tap the right of the page to go on, the left to go back and the top for
 * the menu (page slider, contents, bookmarks, font size or view). The power
 * button puts the Kindle to sleep and wakes on the same page.
 * Usage: books           the library of /mnt/us/documents
 *        books <file> [title]  open one file, and Back leaves (arXiv uses this) */
#include "ui.h"
#include <dirent.h>
#include <pty.h>
#include <termios.h>
#include <signal.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/wait.h>

#define DOCS "/mnt/us/documents"
#define STATE_DIR "/mnt/us/linux/reader"
#define PAGE_PGM "/tmp/inkwell-page.pgm"
#define THUMB_PGM "/tmp/inkwell-thumb.pgm"
#define FOOT 58
#define VIEW_H (H - FOOT)

static const int EMS[] = {20, 24, 27, 30, 34, 38, 44, 52};
#define NEMS (int)(sizeof EMS / sizeof *EMS)

/* ---- the render helper ----------------------------------------------------------- */

static int rin = -1, rout = -1;
static pid_t rpid;
static char rbuf[65536];
static int rlen;

/* MuPDF's answers go through a pseudo-terminal rather than a pipe: on a
 * pipe the C library holds output back in a buffer, and later answers only
 * arrived when mutool quit. A terminal is flushed at every line. */
static void helper_start(void) {
    int to[2], m, sl;
    if (pipe(to) || openpty(&m, &sl, 0, 0, 0)) return;
    struct termios t;
    tcgetattr(sl, &t);
    cfmakeraw(&t);
    tcsetattr(sl, TCSANOW, &t);
    rpid = fork();
    if (rpid == 0) {
        dup2(to[0], 0);
        dup2(sl, 1);
        close(to[1]);
        close(m);
        close(sl);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 2);
        execlp("mutool", "mutool", "run", "/usr/local/share/inkwell/render.js", (char *)0);
        _exit(127);
    }
    close(to[0]);
    close(sl);
    rin = to[1];
    rout = m;
    rlen = 0;
}
static void helper_stop(void) {
    if (rin >= 0) close(rin);
    if (rout >= 0) close(rout);
    if (rpid > 0) { kill(rpid, SIGTERM); waitpid(rpid, 0, 0); }
    rin = rout = -1;
    rpid = 0;
}

static int helper_line(char *out, int n) {
    for (;;) {
        char *nl = memchr(rbuf, '\n', rlen);
        if (nl) {
            int len = nl - rbuf;
            snprintf(out, n, "%.*s", len, rbuf);
            memmove(rbuf, nl + 1, rlen - len - 1);
            rlen -= len + 1;
            return 1;
        }
        if (rlen >= (int)sizeof rbuf - 1) rlen = 0;
        int r = read(rout, rbuf + rlen, sizeof rbuf - 1 - rlen);
        if (r <= 0) return 0;
        rlen += r;
    }
}

/* Sends one command and reads the answer. Returns 1 when it starts with ok. */
static int ask(char *reply, int n, const char *fmt, ...) {
    char cmd[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd - 1, fmt, ap);
    va_end(ap);
    strcat(cmd, "\n");
    if (rin < 0 || write(rin, cmd, strlen(cmd)) < 0) return 0;
    if (!helper_line(reply, n)) return 0;
    return !strncmp(reply, "ok", 2);
}

/* The i-th tab-separated field of a line. */
static void field(const char *line, int i, char *out, int n) {
    const char *p = line;
    while (i-- > 0 && p) { p = strchr(p, '\t'); if (p) p++; }
    if (!p) { out[0] = 0; return; }
    const char *e = strchr(p, '\t');
    int len = e ? (int)(e - p) : (int)strlen(p);
    snprintf(out, n, "%.*s", len, p);
}
static int field_int(const char *line, int i) { char b[32]; field(line, i, b, sizeof b); return atoi(b); }
static float field_float(const char *line, int i) { char b[32]; field(line, i, b, sizeof b); return (float)atof(b); }

/* ---- grey images ------------------------------------------------------------------ */

typedef struct { int w, h; unsigned char *px; } image;

static int read_pgm(const char *path, image *im) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int w = 0, h = 0, mx = 0;
    char magic[3] = "";
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P5")) { fclose(f); return -1; }
    int vals[3], k = 0;
    while (k < 3) {
        int c = fgetc(f);
        if (c == EOF) { fclose(f); return -1; }
        if (c == '#') { while ((c = fgetc(f)) != '\n' && c != EOF) {} continue; }
        if (c >= '0' && c <= '9') { ungetc(c, f); if (fscanf(f, "%d", &vals[k]) == 1) k++; }
    }
    fgetc(f);
    w = vals[0]; h = vals[1]; mx = vals[2];
    if (w <= 0 || h <= 0 || mx != 255) { fclose(f); return -1; }
    free(im->px);
    im->px = malloc((size_t)w * h);
    im->w = w; im->h = h;
    size_t got = fread(im->px, 1, (size_t)w * h, f);
    fclose(f);
    return got == (size_t)w * h ? 0 : -1;
}

/* Copies part of an image into the page at (dx, dy), clipped to the view. */
static void blit(const image *im, int sx, int sy, int w, int h, int dx, int dy) {
    for (int j = 0; j < h; j++) {
        int y = dy + j, iy = sy + j;
        if (y < 0 || y >= VIEW_H || iy < 0 || iy >= im->h) continue;
        for (int i = 0; i < w; i++) {
            int x = dx + i, ix = sx + i;
            if (x < 0 || x >= W || ix < 0 || ix >= im->w) continue;
            fb[y * PITCH + x] = im->px[iy * im->w + ix];
        }
    }
}

/* ---- the book --------------------------------------------------------------------- */

typedef struct {
    char path[512], title[200], author[120];
    int pages, reflow, page, em, mode, part, nparts;
    int marks[100], nmarks;
    long opened;
} book;
static book bk;

static unsigned long fnv(const char *s) {
    unsigned long h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}
static void state_path(const char *doc, char *out, int n) { snprintf(out, n, STATE_DIR "/%08lx.txt", fnv(doc)); }

static void save_state(void) {
    char p[600];
    mkdir(STATE_DIR, 0755);
    state_path(bk.path, p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "path %s\ntitle %s\npage %d\npages %d\nem %d\nmode %d\nopened %ld\n", bk.path, bk.title, bk.page, bk.pages,
            bk.em, bk.mode, bk.opened);
    for (int i = 0; i < bk.nmarks; i++) fprintf(f, "mark %d\n", bk.marks[i]);
    fclose(f);
}

/* Fills in what was saved for a document: page, size, view, bookmarks. */
static void load_state(const char *doc, book *b) {
    char p[600], line[700];
    state_path(doc, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *v = strchr(line, ' ');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "title")) snprintf(b->title, sizeof b->title, "%s", v);
        else if (!strcmp(line, "page")) b->page = atoi(v);
        else if (!strcmp(line, "pages")) b->pages = atoi(v);
        else if (!strcmp(line, "em")) b->em = atoi(v);
        else if (!strcmp(line, "mode")) b->mode = atoi(v);
        else if (!strcmp(line, "opened")) b->opened = atol(v);
        else if (!strcmp(line, "mark") && b->nmarks < 100) b->marks[b->nmarks++] = atoi(v);
    }
    fclose(f);
}

static void file_title(const char *path, char *out, int n) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, n, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    for (char *c = out; *c; c++) if (*c == '_') *c = ' ';
}

static int marked(int page) {
    for (int i = 0; i < bk.nmarks; i++) if (bk.marks[i] == page) return i;
    return -1;
}

/* ---- laying out a page ---------------------------------------------------------- */

/* What the current page shows: one image and, for PDFs, the region of it
 * that each part of the page uses. */
static image pg;
static int pg_page = -1, pg_mode = -1, pg_em = -1;
typedef struct { int x, y, w, h; } region;
static region parts[24];

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* Finds the printed area of a PDF page from a small render, in points. */
static int content_box(int page, float pw, float ph, float *bx0, float *by0, float *bx1, float *by1, float *split) {
    char r[256];
    const float s = 0.4f;
    *bx0 = 0; *by0 = 0; *bx1 = pw; *by1 = ph; *split = -1;
    if (!ask(r, sizeof r, "render\t%d\t%.3f\t%s", page, s, THUMB_PGM)) return -1;
    image t = {0, 0, 0};
    if (read_pgm(THUMB_PGM, &t)) return -1;
    int x0 = t.w, y0 = t.h, x1 = -1, y1 = -1;
    int *colsum = calloc(t.w, sizeof(int));
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++)
            if (t.px[y * t.w + x] < 200) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                colsum[x]++;
            }
    if (x1 > x0 && y1 > y0) {
        *bx0 = clampf((x0 - 4) / s, 0, pw); *bx1 = clampf((x1 + 5) / s, 0, pw);
        *by0 = clampf((y0 - 4) / s, 0, ph); *by1 = clampf((y1 + 5) / s, 0, ph);
        /* a gutter: the emptiest column in the middle third of the text */
        int a = x0 + (x1 - x0) * 35 / 100, b = x0 + (x1 - x0) * 65 / 100, best = -1, bestv = 1 << 30;
        for (int x = a; x <= b; x++) {
            int v = colsum[x] + (x > 0 ? colsum[x - 1] : 0) + (x + 1 < t.w ? colsum[x + 1] : 0);
            if (v < bestv) { bestv = v; best = x; }
        }
        if (best >= 0 && bestv <= (y1 - y0) / 40) *split = best / s;
    }
    free(colsum);
    free(t.px);
    return 0;
}

/* Renders the current page if needed and works out its parts. */
static int prepare(void) {
    char r[256];
    if (pg_page == bk.page && pg_mode == bk.mode && pg_em == bk.em) return 0;
    if (bk.reflow) {
        if (!ask(r, sizeof r, "render\t%d\t1\t%s", bk.page, PAGE_PGM) || read_pgm(PAGE_PGM, &pg)) return -1;
        bk.nparts = 1;
        parts[0] = (region){0, 0, pg.w, pg.h};
    } else {
        if (!ask(r, sizeof r, "bounds\t%d", bk.page)) return -1;
        float pw = field_float(r, 1), ph = field_float(r, 2), x0, y0, x1, y1, split;
        content_box(bk.page, pw, ph, &x0, &y0, &x1, &y1, &split);
        float bw = x1 - x0, bh = y1 - y0;
        if (bk.mode == 0) {
            float s = fminf(W / bw, VIEW_H / bh) * 0.99f;
            if (!ask(r, sizeof r, "render\t%d\t%.4f\t%s", bk.page, s, PAGE_PGM) || read_pgm(PAGE_PGM, &pg)) return -1;
            bk.nparts = 1;
            parts[0] = (region){(int)(x0 * s), (int)(y0 * s), (int)(bw * s), (int)(bh * s)};
        } else {
            /* Columns: each column the width of the screen, cut into screens
             * that overlap a little so no line is lost between them. */
            float cx[3] = {x0, split > 0 ? split : x1, x1};
            int ncol = split > 0 ? 2 : 1;
            float widest = 0;
            for (int c = 0; c < ncol; c++) widest = fmaxf(widest, cx[c + 1] - cx[c]);
            float s = (W - 16) / widest;
            if (!ask(r, sizeof r, "render\t%d\t%.4f\t%s", bk.page, s, PAGE_PGM) || read_pgm(PAGE_PGM, &pg)) return -1;
            int view = VIEW_H, overlap = 60, n = 0, colh = (int)(bh * s);
            for (int c = 0; c < ncol && n < 24; c++) {
                int px0 = (int)(cx[c] * s), pw2 = (int)((cx[c + 1] - cx[c]) * s);
                for (int y = 0; n < 24; y += view - overlap) {
                    int h = colh - y < view ? colh - y : view;
                    parts[n++] = (region){px0, (int)(y0 * s) + y, pw2, h};
                    if (y + view >= colh) break;
                }
            }
            bk.nparts = n;
        }
    }
    pg_page = bk.page; pg_mode = bk.mode; pg_em = bk.em;
    if (bk.part < 0 || bk.part >= bk.nparts) bk.part = bk.part < 0 ? bk.nparts - 1 : 0;
    return 0;
}

/* ---- drawing the page ----------------------------------------------------------- */

static void ribbon(int x, int y, int filled) {
    float pts[] = {x, y, x + 44, y, x + 44, y + 70, x + 22, y + 52, x, y + 70};
    if (filled) poly_fill(pts, 5, BLACK);
    else for (int i = 0; i < 5; i++) line(pts[2 * i], pts[2 * i + 1], pts[(2 * i + 2) % 10], pts[(2 * i + 3) % 10], 3, BLACK);
}

static void draw_footer(void) {
    char right[64], t[220];
    fill(0, VIEW_H, W, FOOT, WHITE);
    int px = MARGIN + (int)((W - 2 * MARGIN) * (bk.page + 1.0f) / (bk.pages ? bk.pages : 1));
    fill(MARGIN, VIEW_H + 6, W - 2 * MARGIN, 2, 0xC0);
    fill(MARGIN, VIEW_H + 5, px - MARGIN, 4, 0x40);
    if (bk.nparts > 1) snprintf(right, sizeof right, "%d of %d  \xC2\xB7  %d/%d", bk.page + 1, bk.pages, bk.part + 1, bk.nparts);
    else snprintf(right, sizeof right, "%d of %d", bk.page + 1, bk.pages);
    text_right(&f_ui, 26, W - MARGIN, VIEW_H + 18, right, 0x50);
    text_fit(&f_ui, 26, bk.title, W - 2 * MARGIN - text_width(&f_ui, 26, right) - 40, t, sizeof t);
    text(&f_ui, 26, MARGIN, VIEW_H + 18, t, 0x50);
}

static void draw_page(void) {
    fill(0, 0, W, VIEW_H, WHITE);
    if (prepare()) {
        text_center(&f_ui_bold, 40, W / 2, VIEW_H / 2 - 30, "This page could not be drawn.", BLACK);
    } else {
        region p = parts[bk.part];
        int dx = (W - p.w) / 2, dy = bk.reflow ? 0 : (VIEW_H - p.h) / 2;
        if (!bk.reflow && bk.mode == 1) dy = 0;
        blit(&pg, p.x, p.y, p.w, p.h, dx, dy);
    }
    if (marked(bk.page) >= 0) ribbon(W - 90, 0, 1);
    draw_footer();
}

/* ---- lists: library, contents, bookmarks ------------------------------------------ */

#define ROW_H 120
#define LIST_Y 170

typedef struct { char label[220], sub[120]; int value, depth; } item;

/* Shows a paged list and returns the index tapped, or -1 for Back. */
static int pick(const char *title, item *it, int n, const char *empty, int start) {
    int per = (H - LIST_Y - 150) / ROW_H, top = start >= 0 ? start / per * per : 0;
    const rect prev_b = {MARGIN, H - 120, 220, 90}, next_b = {W - MARGIN - 220, H - 120, 220, 90};
    for (;;) {
        header(title);
        if (!n) text(&f_ui, 36, MARGIN, LIST_Y + 40, empty, 0x50);
        for (int i = top; i < n && i < top + per; i++) {
            int y = LIST_Y + (i - top) * ROW_H, x = MARGIN + it[i].depth * 34;
            char fit[240];
            int subw = it[i].sub[0] ? text_width(&f_ui, 28, it[i].sub) + 30 : 0;
            text_fit(it[i].depth ? &f_ui : &f_ui_bold, 36, it[i].label, W - MARGIN - x - subw, fit, sizeof fit);
            text(it[i].depth ? &f_ui : &f_ui_bold, 36, x, y + 22, fit, BLACK);
            if (it[i].sub[0]) text_right(&f_ui, 28, W - MARGIN, y + 30, it[i].sub, 0x60);
            fill(MARGIN, y + ROW_H - 2, W - 2 * MARGIN, 1, 0xB0);
        }
        if (top > 0) pill(prev_b, "Previous", 0);
        if (top + per < n) pill(next_b, "Next", 0);
        if (n > per) {
            char pgs[32];
            snprintf(pgs, sizeof pgs, "%d / %d", top / per + 1, (n + per - 1) / per);
            text_center(&f_ui, 30, W / 2, H - 92, pgs, 0x50);
        }
        flip();
        for (;;) {
            event e = input_wait(-1);
            if (e.type == EV_POWER) { sleep_now(); continue; }
            if (e.type == EV_DOWN) {
                if (hit(back_btn, e.x, e.y)) press_on(back_btn);
                else if (top > 0 && hit(prev_b, e.x, e.y)) press_on(prev_b);
                else if (top + per < n && hit(next_b, e.x, e.y)) press_on(next_b);
                else if (e.y >= LIST_Y && e.y < LIST_Y + per * ROW_H) {
                    int i = top + (e.y - LIST_Y) / ROW_H;
                    if (i < n) press_on((rect){0, LIST_Y + (i - top) * ROW_H, W, ROW_H - 2});
                }
                continue;
            }
            if (e.type != EV_TAP) continue;
            press_forget();
            if (hit(back_btn, e.x, e.y)) return -1;
            if (top > 0 && hit(prev_b, e.x, e.y)) { top -= per; break; }
            if (top + per < n && hit(next_b, e.x, e.y)) { top += per; break; }
            if (e.y >= LIST_Y && e.y < LIST_Y + per * ROW_H) {
                int i = top + (e.y - LIST_Y) / ROW_H;
                if (i < n) return i;
            }
            press_off();
        }
    }
}

static int show_contents(void) {
    static item it[400];
    char line[600];
    int n = 0;
    if (rin < 0) return -1;
    if (write(rin, "outline\n", 8) < 0) return -1;
    while (helper_line(line, sizeof line)) {
        if (!strncmp(line, "ok", 2) || !strncmp(line, "err", 3)) break;
        if (line[0] != 'o' || n >= 400) continue;
        it[n].depth = field_int(line, 1);
        if (it[n].depth > 3) it[n].depth = 3;
        it[n].value = field_int(line, 2);
        field(line, 3, it[n].label, sizeof it[n].label);
        if (it[n].value >= 0) snprintf(it[n].sub, sizeof it[n].sub, "%d", it[n].value + 1);
        else it[n].sub[0] = 0;
        n++;
    }
    int k = pick("Contents", it, n, "This book has no table of contents.", -1);
    return k >= 0 && it[k].value >= 0 ? it[k].value : -1;
}

static int show_marks(void) {
    static item it[100];
    char r[400];
    for (int a = 1; a < bk.nmarks; a++) /* in page order */
        for (int b = a; b > 0 && bk.marks[b - 1] > bk.marks[b]; b--) { int t = bk.marks[b]; bk.marks[b] = bk.marks[b - 1]; bk.marks[b - 1] = t; }
    for (int i = 0; i < bk.nmarks; i++) {
        it[i].depth = 0;
        it[i].value = bk.marks[i];
        if (ask(r, sizeof r, "text\t%d", bk.marks[i])) field(r, 1, it[i].label, sizeof it[i].label);
        if (!it[i].label[0]) snprintf(it[i].label, sizeof it[i].label, "Page %d", bk.marks[i] + 1);
        snprintf(it[i].sub, sizeof it[i].sub, "page %d", bk.marks[i] + 1);
    }
    int k = pick("Bookmarks", it, bk.nmarks, "No bookmarks yet. Open the menu and tap the ribbon to add one.", -1);
    return k >= 0 ? it[k].value : -1;
}

/* ---- the menu --------------------------------------------------------------------- */

#define MENU_Y (H - 330)
static rect m_mark, m_slider, m_b[4];
static int m_nb;

static void draw_menu(void) {
    char t[200], pgline[64];
    fill(0, 0, W, HEADER_H, WHITE);
    float cx = back_btn.x + 52, cy = back_btn.y + back_btn.h / 2.0f;
    ring(cx, cy, 34, 3, BLACK);
    line(cx + 8, cy - 15, cx - 8, cy, 5, BLACK);
    line(cx - 8, cy, cx + 8, cy + 15, 5, BLACK);
    text_fit(&f_serif_bold, 56, bk.title, W - 330, t, sizeof t);
    text(&f_serif_bold, 56, back_btn.x + 110, 46, t, BLACK);
    m_mark = (rect){W - 140, 20, 110, 110};
    ribbon(W - 108, 40, marked(bk.page) >= 0);
    fill(0, HEADER_H - 2, W, 2, BLACK);

    fill(0, MENU_Y, W, H - MENU_Y, WHITE);
    fill(0, MENU_Y, W, 2, BLACK);
    snprintf(pgline, sizeof pgline, "Page %d of %d", bk.page + 1, bk.pages);
    text(&f_ui_bold, 32, MARGIN, MENU_Y + 28, pgline, BLACK);
    m_slider = (rect){MARGIN, MENU_Y + 80, W - 2 * MARGIN, 80};
    int y = m_slider.y + 40, kx = MARGIN + (int)((W - 2 * MARGIN) * (bk.pages > 1 ? bk.page / (float)(bk.pages - 1) : 0));
    rrect_fill(MARGIN, y - 5, W - 2 * MARGIN, 10, 5, 0xC0);
    rrect_fill(MARGIN, y - 5, kx - MARGIN, 10, 5, BLACK);
    disc(kx, y, 22, BLACK);
    disc(kx, y, 11, WHITE);
    const char *lab[4];
    m_nb = 0;
    if (bk.reflow) { lab[m_nb++] = "A\xE2\x88\x92"; lab[m_nb++] = "A+"; }
    else lab[m_nb++] = bk.mode ? "Whole page" : "Columns";
    lab[m_nb++] = "Contents";
    lab[m_nb++] = "Bookmarks";
    int gap = 20, bw = (W - 2 * MARGIN - (m_nb - 1) * gap) / m_nb;
    for (int i = 0; i < m_nb; i++) {
        m_b[i] = (rect){MARGIN + i * (bw + gap), H - 130, bw, 96};
        pill(m_b[i], lab[i], 0);
    }
}

/* ---- reading ---------------------------------------------------------------------- */

static const char *title_hint; /* from arXiv, whose PDFs carry no title */
static int open_book(const char *path) {
    char r[1200];
    memset(&bk, 0, sizeof bk);
    snprintf(bk.path, sizeof bk.path, "%s", path);
    bk.em = 30;
    load_state(path, &bk);
    int saved_pages = bk.pages;
    if (!ask(r, sizeof r, "open\t%s\t%d\t%d\t%d", path, W, VIEW_H, bk.em)) return -1;
    bk.pages = field_int(r, 1);
    bk.reflow = field_int(r, 2);
    char title[200], author[120];
    field(r, 3, title, sizeof title);
    field(r, 4, author, sizeof author);
    if (title[0]) snprintf(bk.title, sizeof bk.title, "%s", title);
    if (title_hint && title_hint[0]) snprintf(bk.title, sizeof bk.title, "%s", title_hint);
    if (!bk.title[0]) file_title(path, bk.title, sizeof bk.title);
    snprintf(bk.author, sizeof bk.author, "%s", author);
    /* the same font size gives the same pages; otherwise keep the place by fraction */
    if (saved_pages && saved_pages != bk.pages && bk.reflow) bk.page = (int)((float)bk.page / saved_pages * bk.pages);
    if (bk.page >= bk.pages) bk.page = bk.pages - 1;
    if (bk.page < 0) bk.page = 0;
    bk.opened = (long)time(0);
    pg_page = -1;
    save_state();
    return 0;
}

static void relayout(int em_index) {
    char r[128];
    int old = bk.pages ? bk.pages : 1;
    float frac = (float)bk.page / old;
    bk.em = EMS[em_index];
    if (ask(r, sizeof r, "layout\t%d\t%d\t%d", W, VIEW_H, bk.em)) {
        bk.pages = field_int(r, 1);
        /* bookmarks keep their place in the book, not their page number */
        for (int i = 0; i < bk.nmarks; i++) bk.marks[i] = (int)((float)bk.marks[i] / old * bk.pages);
        bk.page = (int)(frac * bk.pages + 0.5f);
        if (bk.page >= bk.pages) bk.page = bk.pages - 1;
        if (bk.page < 0) bk.page = 0;
    }
    bk.part = 0;
    pg_page = -1;
}

static int em_index(void) {
    int k = 0;
    for (int i = 0; i < NEMS; i++) if (EMS[i] <= bk.em) k = i;
    return k;
}

static void go(int page, int part) {
    bk.page = page;
    bk.part = part;
    save_state();
    draw_page();
    flip();
}

/* The reading loop for the open book. Returns when the reader taps Back. */
static void read_book(void) {
    int menu = 0;
    draw_page();
    flip_flash();
    for (;;) {
        event e = input_wait(-1);
        if (e.type == EV_NONE) continue;
        if (e.type == EV_POWER) { sleep_now(); continue; }
        if (menu && e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            for (int i = 0; i < m_nb; i++) if (hit(m_b[i], e.x, e.y)) press_on(m_b[i]);
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (!menu) {
            if (e.y < 150) { /* the menu */
                menu = 1;
                draw_menu();
                refresh(0, 0, W, H, WF_GC16, 0, 0);
            } else if (e.x < W * 3 / 10) { /* back a page */
                if (bk.part > 0) go(bk.page, bk.part - 1);
                else if (bk.page > 0) go(bk.page - 1, -1);
            } else { /* on */
                if (bk.part < bk.nparts - 1) go(bk.page, bk.part + 1);
                else if (bk.page < bk.pages - 1) go(bk.page + 1, 0);
                else { toast("That's the end."); }
            }
            continue;
        }
        /* the menu is open */
        if (hit(back_btn, e.x, e.y)) { press_forget(); save_state(); return; }
        if (hit(m_mark, e.x, e.y)) {
            int k = marked(bk.page);
            if (k >= 0) bk.marks[k] = bk.marks[--bk.nmarks];
            else if (bk.nmarks < 100) bk.marks[bk.nmarks++] = bk.page;
            save_state();
            draw_menu();
            refresh(m_mark.x, m_mark.y, m_mark.w, m_mark.h, WF_GC16, 0, 0);
            continue;
        }
        if (hit(m_slider, e.x, e.y)) {
            float f = clampf((e.ux - MARGIN) / (float)(W - 2 * MARGIN), 0, 1);
            bk.page = (int)(f * (bk.pages - 1) + 0.5f);
            bk.part = 0;
            save_state();
            draw_page();
            draw_menu();
            refresh(0, 0, W, H, WF_GC16, 0, 0);
            continue;
        }
        int b = -1;
        for (int i = 0; i < m_nb; i++) if (hit(m_b[i], e.x, e.y)) b = i;
        press_forget();
        if (b < 0) { /* anywhere else closes the menu */
            if (e.y > HEADER_H && e.y < MENU_Y) { menu = 0; draw_page(); refresh(0, 0, W, H, WF_GC16, 0, 0); }
            continue;
        }
        if (bk.reflow && b <= 1) {
            int k = em_index() + (b == 0 ? -1 : 1);
            if (k < 0 || k >= NEMS) { draw_menu(); refresh(0, MENU_Y, W, H - MENU_Y, WF_GC16, 0, 0); continue; }
            toast("Laying out the book...");
            relayout(k);
            save_state();
            draw_page();
            draw_menu();
            flip();
            continue;
        }
        if (!bk.reflow && b == 0) {
            bk.mode = !bk.mode;
            bk.part = 0;
            save_state();
            draw_page();
            draw_menu();
            refresh(0, 0, W, H, WF_GC16, 0, 0);
            continue;
        }
        int is_contents = (bk.reflow && b == 2) || (!bk.reflow && b == 1);
        int target = is_contents ? show_contents() : show_marks();
        if (target >= 0 && target < bk.pages) { bk.page = target; bk.part = 0; save_state(); }
        menu = 0;
        draw_page();
        flip();
    }
}

/* ---- the library ------------------------------------------------------------------ */

static int supported(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *ext[] = {".pdf", ".epub", ".txt", ".mobi", ".fb2", ".cbz", ".xps"};
    for (int i = 0; i < 7; i++) if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

typedef struct { char path[512]; book b; } entry;
static entry lib[400];
static int nlib;

static void scan(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && nlib < 400) {
        if (de->d_name[0] == '.') continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            size_t l = strlen(de->d_name);
            if (depth < 2 && !(l > 4 && !strcmp(de->d_name + l - 4, ".sdr"))) scan(p, depth + 1);
        } else if (supported(de->d_name)) {
            entry *e = &lib[nlib++];
            memset(e, 0, sizeof *e);
            snprintf(e->path, sizeof e->path, "%s", p);
            load_state(p, &e->b);
            if (!e->b.title[0]) file_title(p, e->b.title, sizeof e->b.title);
        }
    }
    closedir(d);
}

static int lib_cmp(const void *a, const void *b) {
    const entry *x = a, *y = b;
    if (x->b.opened != y->b.opened) return x->b.opened < y->b.opened ? 1 : -1;
    return strcasecmp(x->b.title, y->b.title);
}

static void library(void) {
    static item it[400];
    for (;;) {
        nlib = 0;
        scan(DOCS, 0);
        qsort(lib, nlib, sizeof *lib, lib_cmp);
        for (int i = 0; i < nlib; i++) {
            const char *dot = strrchr(lib[i].path, '.');
            char fmt[8] = "";
            for (int k = 0; dot && dot[k + 1] && k < 6; k++) fmt[k] = (char)(dot[k + 1] >= 'a' ? dot[k + 1] - 32 : dot[k + 1]), fmt[k + 1] = 0;
            snprintf(it[i].label, sizeof it[i].label, "%s", lib[i].b.title);
            if (lib[i].b.pages) snprintf(it[i].sub, sizeof it[i].sub, "%s  %d%%", fmt, (lib[i].b.page + 1) * 100 / lib[i].b.pages);
            else snprintf(it[i].sub, sizeof it[i].sub, "%s  new", fmt);
            it[i].depth = 0;
        }
        int k = pick("Books", it, nlib, "Copy EPUB, PDF or text files into documents.", -1);
        if (k < 0) return;
        toast("Opening...");
        if (open_book(lib[k].path)) { toast("This file could not be opened."); sleep(2); continue; }
        read_book();
    }
}

int main(int argc, char **argv) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    signal(SIGPIPE, SIG_IGN);
    helper_start();
    if (argc > 1) {
        if (argc > 2) title_hint = argv[2];
        if (open_book(argv[1])) {
            header("Books");
            text(&f_ui, 36, MARGIN, 220, "This file could not be opened.", BLACK);
            flip();
            sleep(2);
        } else read_book();
    } else library();
    helper_stop();
    return 0;
}

/* eink.h: the display and input layer of Inkwell, our own Kindle userspace.
 * /dev/fb0 with the Lab126 EPDC refresh ioctl, TrueType text through
 * stb_truetype with a glyph cache, the Inkwell typefaces, and queued touch
 * and power button events. Include it once. */
#ifndef EINK_H
#define EINK_H
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

/* ---- framebuffer and refresh -------------------------------------------- */

struct mxcfb_rect { unsigned top, left, width, height; };
struct mxcfb_update_data {
    struct mxcfb_rect region;
    unsigned waveform, mode, marker, hist_bw, hist_gray;
    int temp;
    unsigned flags;
    unsigned alt[7];
};
/* _IOW('F', 0x2E, 72 bytes). The PW3 kernel rejects the 76-byte layout. */
#define MXCFB_SEND_UPDATE 0x4048462Eu
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE 0xC008462Fu
/* Measured on this panel: A2 190 ms (black and white only), DU 380 ms,
 * GC16 and GL16 660 ms. The size of the region makes no difference. */
#define WF_DU 1
#define WF_GC16 2
#define WF_A2 4
#define WF_GL16 5
#define TEMP_AMBIENT 0x1000

#define BLACK 0x00
#define WHITE 0xFF

static int fbfd = -1, W, H, PITCH;
static unsigned char *fb;
static unsigned marker;

static int fb_open(void) {
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &v) || ioctl(fbfd, FBIOGET_FSCREENINFO, &f)) return -1;
    if (v.bits_per_pixel != 8) { fprintf(stderr, "expected 8 bpp, got %u\n", v.bits_per_pixel); return -1; }
    W = v.xres; H = v.yres; PITCH = f.line_length;
    unsigned char *m = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (m == MAP_FAILED) return -1;
    fb = m + v.yoffset * PITCH;
    return 0;
}

/* full=1 flashes the panel and clears ghosting, full=0 changes only what
 * changed. The driver copies the region when the update is sent, so drawing
 * can carry on straight away and wait is almost never needed. */
static void refresh(int x, int y, int w, int h, int wf, int full, int wait) {
    struct mxcfb_update_data u;
    memset(&u, 0, sizeof u);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    u.region = (struct mxcfb_rect){y, x, w, h};
    u.waveform = wf;
    u.mode = full;
    u.marker = ++marker;
    u.hist_bw = WF_DU;
    u.hist_gray = WF_GC16;
    u.temp = TEMP_AMBIENT;
    if (ioctl(fbfd, MXCFB_SEND_UPDATE, &u) < 0) { perror("SEND_UPDATE"); return; }
    if (wait) {
        unsigned m[2] = {marker, 0};
        ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, m);
    }
}
static void refresh_all(void) { refresh(0, 0, W, H, WF_GC16, 1, 1); }

/* A screen change. Partial GC16 only redraws pixels that changed, which is
 * quicker and calmer than a flash. Every few pages a full flash clears the
 * ghosting that partial updates leave behind. */
static int pages_since_flash;
static void flip(void) {
    if (++pages_since_flash >= 8) { pages_since_flash = 0; refresh(0, 0, W, H, WF_GC16, 1, 0); }
    else refresh(0, 0, W, H, WF_GC16, 0, 0);
}
static void flip_flash(void) { pages_since_flash = 0; refresh(0, 0, W, H, WF_GC16, 1, 0); }

static void fill(int x, int y, int w, int h, unsigned char g) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) memset(fb + (y + j) * PITCH + x, g, w);
}
static void frame(int x, int y, int w, int h, int t, unsigned char g) {
    fill(x, y, w, t, g); fill(x, y + h - t, w, t, g);
    fill(x, y, t, h, g); fill(x + w - t, y, t, h, g);
}
static void invert(int x, int y, int w, int h) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) fb[j * PITCH + i] ^= 0xFF;
}

/* Saves and restores a rectangle of the page, for pressed states and popups. */
typedef struct { int x, y, w, h; unsigned char *px; } snapshot;
static snapshot snap(int x, int y, int w, int h) {
    snapshot s = {x, y, w, h, malloc((size_t)w * h)};
    for (int j = 0; j < h; j++) memcpy(s.px + j * w, fb + (y + j) * PITCH + x, w);
    return s;
}
static void unsnap(snapshot *s) {
    if (!s->px) return;
    for (int j = 0; j < s->h; j++) memcpy(fb + (s->y + j) * PITCH + s->x, s->px + j * s->w, s->w);
    free(s->px);
    s->px = 0;
}

/* ---- text ----------------------------------------------------------------- */

typedef struct { stbtt_fontinfo info; const char *file; int loaded; } font;

/* The Inkwell type set. Inter for the interface, Cormorant Garamond for
 * display, Caveat for handwriting, JetBrains Mono for editing and the
 * terminal, and DejaVu Sans for the chess pieces. */
static font f_ui = {.file = "Inter-Regular.ttf"};
static font f_ui_bold = {.file = "Inter-SemiBold.ttf"};
static font f_heavy = {.file = "Inter-ExtraBold.ttf"};
static font f_serif = {.file = "Cormorant-Medium.ttf"};
static font f_serif_bold = {.file = "Cormorant-Bold.ttf"};
static font f_serif_italic = {.file = "Cormorant-Italic.ttf"};
static font f_hand = {.file = "Caveat-Medium.ttf"};
static font f_mono = {.file = "JetBrainsMono-Regular.ttf"};
static font f_chess = {.file = "DejaVuSans-Chess.ttf"}; /* only the twelve chess symbols */
#define FONT_DIR "/usr/share/fonts/inkwell/"

/* Fonts are memory-mapped on first use, so an app only pays for the faces it
 * draws with and the page cache shares them between apps. */
static font *use(font *f) {
    if (f->loaded) return f;
    char path[160];
    snprintf(path, sizeof path, FONT_DIR "%s", f->file);
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0) {
        void *m = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED && stbtt_InitFont(&f->info, m, 0)) f->loaded = 1;
    }
    if (fd >= 0) close(fd);
    if (!f->loaded && f != &f_ui) { fprintf(stderr, "no font %s, using Inter\n", path); *f = *use(&f_ui); }
    return f;
}
static int fonts_load(void) { return use(&f_ui)->loaded ? 0 : -1; }

/* Decodes one UTF-8 code point and advances *s. */
static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++, n = 0;
    if (c >= 0xF0) { c &= 7; n = 3; } else if (c >= 0xE0) { c &= 15; n = 2; } else if (c >= 0xC0) { c &= 31; n = 1; }
    while (n-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 63);
    *s = (const char *)p;
    return c;
}

/* Rasterised glyphs are cached by font, size and code point, since the
 * keyboard and lists redraw the same few glyphs over and over. */
typedef struct { const font *f; int px, c, w, h, ox, oy, adv; unsigned char *bm; } glyph;
#define GLYPH_CACHE 2048
static glyph glyph_cache[GLYPH_CACHE];

static const glyph *glyph_get(font *f, int px, int c) {
    use(f);
    unsigned hsh = ((unsigned)(size_t)f * 31u + (unsigned)px * 131u + (unsigned)c * 2654435761u) % GLYPH_CACHE;
    glyph *g = &glyph_cache[hsh];
    if (g->bm && g->f == f && g->px == px && g->c == c) return g;
    if (g->bm) stbtt_FreeBitmap(g->bm, 0);
    float sc = stbtt_ScaleForPixelHeight(&f->info, px);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->info, c, &adv, &lsb);
    g->f = f; g->px = px; g->c = c;
    g->adv = (int)(adv * sc + 0.5f);
    g->bm = stbtt_GetCodepointBitmap(&f->info, sc, sc, c, &g->w, &g->h, &g->ox, &g->oy);
    if (!g->bm) g->bm = calloc(1, 1), g->w = g->h = 0;
    return g;
}

static int text_width(font *f, int px, const char *t) {
    int x = 0;
    while (*t) x += glyph_get(f, px, utf8_next(&t))->adv;
    return x;
}

static int font_ascent(font *f, int px) {
    int asc, desc, gap;
    use(f);
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    return (int)(asc * stbtt_ScaleForPixelHeight(&f->info, px) + 0.5f);
}

/* Draws text with its top-left corner at (x, y), blending grey ink onto the
 * page. Returns the x where the text ends. */
static int text(font *f, int px, int x, int y, const char *t, unsigned char ink) {
    int base = y + font_ascent(f, px), pen = x;
    while (*t) {
        const glyph *g = glyph_get(f, px, utf8_next(&t));
        for (int j = 0; j < g->h; j++) {
            int yy = base + g->oy + j;
            if (yy < 0 || yy >= H) continue;
            const unsigned char *src = g->bm + j * g->w;
            unsigned char *row = fb + yy * PITCH;
            for (int i = 0; i < g->w; i++) {
                int xx = pen + g->ox + i, a = src[i];
                if (!a || xx < 0 || xx >= W) continue;
                row[xx] = a == 255 ? ink : (unsigned char)(row[xx] + (ink - row[xx]) * a / 255);
            }
        }
        pen += g->adv;
    }
    return pen;
}
static void text_center(font *f, int px, int cx, int y, const char *t, unsigned char ink) {
    text(f, px, cx - text_width(f, px, t) / 2, y, t, ink);
}
static void text_right(font *f, int px, int rx, int y, const char *t, unsigned char ink) {
    text(f, px, rx - text_width(f, px, t), y, t, ink);
}

/* Cuts text with an ellipsis so it fits in max_w pixels. */
static void text_fit(font *f, int px, const char *t, int max_w, char *out, int n) {
    snprintf(out, n, "%s", t);
    if (text_width(f, px, out) <= max_w) return;
    int len = strlen(out);
    while (len > 0) {
        while (len > 0 && (out[len - 1] & 0xC0) == 0x80) len--;
        if (len > 0) len--;
        snprintf(out + len, n - len, "...");
        if (text_width(f, px, out) <= max_w) return;
    }
}

/* Word-wraps text into lines no wider than max_w. Calls emit for each line
 * (which may be NULL to only count) and returns the number of lines. */
static int text_wrap(font *f, int px, const char *t, int max_w, void (*emit)(const char *line, int n, void *ctx), void *ctx) {
    int lines = 0;
    while (*t) {
        const char *start = t, *brk = 0, *p = t;
        int w = 0;
        while (*p && *p != '\n') {
            const char *q = p;
            int cw = glyph_get(f, px, utf8_next(&q))->adv;
            if (w + cw > max_w && p > start) break;
            if (*p == ' ') brk = p;
            w += cw;
            p = q;
        }
        const char *end = p;
        if (*p && *p != '\n' && brk) end = brk;
        if (emit) emit(start, (int)(end - start), ctx);
        lines++;
        t = end;
        if (*t == ' ' || *t == '\n') t++;
    }
    return lines;
}

/* ---- input ---------------------------------------------------------------- */

static int touchfd = -1, keyfd = -1;
static int tx0, tx1, ty0, ty1;

static void input_open(void) {
    for (int n = 0; n < 8; n++) {
        char path[32], name[64] = "";
        snprintf(path, sizeof path, "/dev/input/event%d", n);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        struct input_absinfo ax, ay;
        if (touchfd < 0 && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 &&
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0 && ax.maximum > 0) {
            touchfd = fd; tx0 = ax.minimum; tx1 = ax.maximum; ty0 = ay.minimum; ty1 = ay.maximum;
        } else if (keyfd < 0 && strstr(name, "onkey")) {
            keyfd = fd;
        } else {
            close(fd);
        }
    }
}

/* EV_DOWN when a finger lands, EV_MOVE while it drags, EV_TAP when it lifts
 * (with the position where it landed, plus where it lifted in ux, uy). */
enum { EV_NONE, EV_TAP, EV_POWER, EV_DOWN, EV_MOVE };
typedef struct { int type, x, y, ux, uy; } event;

/* Events are queued, because several can arrive while the panel is busy
 * refreshing, and a fast typist would otherwise lose keys. */
#define EVQ 256
static event evq[EVQ];
static int evq_head, evq_tail;
static int want_moves;
static void evq_push(event e) {
    int next = (evq_tail + 1) % EVQ;
    if (next != evq_head) { evq[evq_tail] = e; evq_tail = next; }
}

static void input_drain(void) {
    struct input_event ev[64];
    while (touchfd >= 0 && read(touchfd, ev, sizeof ev) > 0) {}
    while (keyfd >= 0 && read(keyfd, ev, sizeof ev) > 0) {}
    evq_head = evq_tail = 0;
}

static void input_read(void) {
    static int down, x, y, sx, sy, moved;
    struct input_event ev[64];
    long n;
    while (keyfd >= 0 && (n = read(keyfd, ev, sizeof ev)) > 0)
        for (int i = 0; i < n / (long)sizeof *ev; i++)
            if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1) evq_push((event){EV_POWER, 0, 0, 0, 0});
    while (touchfd >= 0 && (n = read(touchfd, ev, sizeof ev)) > 0)
        for (int i = 0; i < n / (long)sizeof *ev; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_ABS) {
                if (e->code == ABS_MT_POSITION_X) x = (e->value - tx0) * (W - 1) / (tx1 - tx0 ? tx1 - tx0 : 1), moved = 1;
                else if (e->code == ABS_MT_POSITION_Y) y = (e->value - ty0) * (H - 1) / (ty1 - ty0 ? ty1 - ty0 : 1), moved = 1;
                else if (e->code == ABS_MT_TRACKING_ID) {
                    /* The kernel drops coordinates that didn't change, so a finger
                     * landing where the last one lifted sends none. Count the
                     * landing itself as movement. */
                    if (e->value >= 0 && !down) { down = 1; sx = -1; moved = 1; }
                    else if (e->value < 0 && down) {
                        down = 0;
                        evq_push((event){EV_TAP, sx >= 0 ? sx : x, sx >= 0 ? sy : y, x, y});
                    }
                }
            } else if (e->type == EV_SYN && e->code == SYN_REPORT && down && moved) {
                moved = 0;
                if (sx < 0) { sx = x; sy = y; evq_push((event){EV_DOWN, x, y, x, y}); }
                else if (want_moves) evq_push((event){EV_MOVE, x, y, x, y});
            }
        }
}

/* Waits up to timeout_ms (-1 for ever) for the next event. */
static event input_wait(int timeout_ms) {
    if (evq_head == evq_tail) {
        struct pollfd p[2] = {{touchfd, POLLIN, 0}, {keyfd, POLLIN, 0}};
        if (poll(p, 2, timeout_ms) > 0) input_read();
    }
    if (evq_head == evq_tail) return (event){EV_NONE, 0, 0, 0, 0};
    event e = evq[evq_head];
    evq_head = (evq_head + 1) % EVQ;
    return e;
}

#endif

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
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "board.h"

/* ---- framebuffer and refresh -------------------------------------------- */

struct mxcfb_rect { unsigned top, left, width, height; };
/* The PW3's Lab126 layout, _IOW('F', 0x2E, 72 bytes). Its kernel rejects 76. */
struct mxcfb_update_data {
    struct mxcfb_rect region;
    unsigned waveform, mode, marker, hist_bw, hist_gray;
    int temp;
    unsigned flags;
    unsigned alt[7];
};
/* The PW4 kernel takes only 80 bytes, in NXP's order: temp and flags come
 * straight after the marker. In the PW3 order, hist_gray = 2 lands on flags
 * as EPDC_FLAG_FORCE_MONOCHROME and every grey turns black or white. */
struct mxcfb_update_data_pw4 {
    struct mxcfb_rect region;
    unsigned waveform, mode, marker;
    int temp;
    unsigned flags;
    int dither_mode, quant_bit;
    unsigned alt[7];
    unsigned rest[2];
};
#define MXCFB_SEND_UPDATE 0x4048462Eu
#define MXCFB_SEND_UPDATE_PW4 0x4050462Eu
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE 0xC008462Fu
/* Measured on the PW3: A2 190 ms (black and white only), DU 380 ms,
 * GC16 and GL16 660 ms. The size of the region makes no difference.
 * These are the PW3's numbers. The PW4 numbers its waveforms differently
 * (A2 6, GL16 3, REAGL 4), and refresh() translates. Measured there: A2
 * 188 ms, DU 370 ms, GL16 and REAGL 650 ms, GC16 750 ms. */
#define WF_DU 1
#define WF_GC16 2
#define WF_A2 4
#define WF_GL16 5
#define TEMP_AMBIENT 0x1000

#define WF_PW4_REAGL 4

/* The PW4's panel ghosts under partial GC16 and GL16. Amazon turns pages
 * there with REAGL, which cleans up after the previous image without a
 * flash, so partial grey updates use it. Full updates stay GC16. */
static unsigned wf_board(int wf, int full) {
    if (!board_pw4()) return wf;
    if (wf == WF_A2) return 6;
    if (!full && (wf == WF_GC16 || wf == WF_GL16)) return WF_PW4_REAGL;
    return wf == WF_GL16 ? 3 : wf;
}

#define BLACK 0x00
#define WHITE 0xFF

static int fbfd = -1, W, H, PITCH;
static unsigned char *fb;
static unsigned marker;

static void wait_last_update(void);
static int fb_open(void) {
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &v) || ioctl(fbfd, FBIOGET_FSCREENINFO, &f)) return -1;
    /* The PW4 panel is mounted sideways, and upright is rotate 3. Amazon's X
     * server also sets grayscale 2, which inverts the panel (0xFF shows
     * black), and draws inverted to match. We want 0xFF white as on the
     * PW3, which is grayscale 1. The first app after boot sets both and the
     * later apps find them set. */
    if (board_pw4() && (v.rotate != 3 || v.grayscale != 1 || v.bits_per_pixel != 8)) {
        v.rotate = 3;
        v.grayscale = 1;
        v.bits_per_pixel = 8;
        if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &v)) perror("FBIOPUT_VSCREENINFO");
        if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) || ioctl(fbfd, FBIOGET_FSCREENINFO, &f)) return -1;
    }
    if (v.bits_per_pixel != 8) { fprintf(stderr, "expected 8 bpp, got %u\n", v.bits_per_pixel); return -1; }
    W = v.xres; H = v.yres; PITCH = f.line_length;
    unsigned char *m = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (m == MAP_FAILED) return -1;
    fb = m + v.yoffset * PITCH;
    /* leave the panel idle for whichever app draws next */
    static int hooked;
    if (!hooked) { hooked = 1; atexit(wait_last_update); }
    return 0;
}

/* The app's name, for the log (own.log collects every app's stderr). */
static const char *app_name(void) {
    static char name[32];
    if (!name[0]) {
        FILE *f = fopen("/proc/self/comm", "r");
        if (!f || !fgets(name, sizeof name, f)) snprintf(name, sizeof name, "pid %d", (int)getpid());
        if (f) fclose(f);
        name[strcspn(name, "\n")] = 0;
    }
    return name;
}

/* Update markers name an update to the driver, which serves every app. Each
 * app used to count from 1, so home, the app it opened and the next one all
 * reused the same few numbers. Starting from the pid keeps them apart (FBInk
 * does the same). */
static unsigned next_marker(void) {
    if (!marker) marker = ((unsigned)getpid() & 0xFFFFFu) << 10;
    if (++marker == 0) marker = 1;
    return marker;
}

/* Sends one update and returns its marker, or 0 if the driver refused it.
 * The driver can turn an update away for a moment (powering the panel back
 * up after idling, or its queue full), so a refusal is retried for up to
 * about a second and a half before giving up. A wrong struct (EINVAL) isn't
 * retried, since it never works. */
static unsigned send_update(int x, int y, int w, int h, int wf, int full) {
    for (int tries = 0;; tries++) {
        unsigned mk = next_marker();
        int r;
        if (board_pw4()) {
            struct mxcfb_update_data_pw4 u;
            memset(&u, 0, sizeof u);
            u.region = (struct mxcfb_rect){y, x, w, h};
            u.waveform = wf_board(wf, full);
            /* REAGL only works as a full update: sent as a partial one, the driver
             * leaves some pixels as they were, and the last page shows through.
             * A full REAGL update still doesn't flash. */
            u.mode = full || u.waveform == WF_PW4_REAGL;
            u.marker = mk;
            u.temp = TEMP_AMBIENT;
            r = ioctl(fbfd, MXCFB_SEND_UPDATE_PW4, &u);
        } else {
            struct mxcfb_update_data u;
            memset(&u, 0, sizeof u);
            u.region = (struct mxcfb_rect){y, x, w, h};
            u.waveform = wf;
            u.mode = full;
            u.marker = mk;
            u.hist_bw = WF_DU;
            u.hist_gray = WF_GC16;
            u.temp = TEMP_AMBIENT;
            r = ioctl(fbfd, MXCFB_SEND_UPDATE, &u);
        }
        if (r >= 0) {
            if (tries) fprintf(stderr, "%s: screen update went through after %d retries\n", app_name(), tries);
            return mk;
        }
        int e = errno;
        if (e == EINVAL || e == ENOTTY || e == EFAULT || e == EBADF || tries >= 10) {
            fprintf(stderr, "%s: screen update %dx%d+%d+%d wf %d refused: %s\n", app_name(), w, h, x, y, wf, strerror(e));
            return 0;
        }
        usleep(30000 * (tries + 1));
    }
}

static long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* The last update sent, and when: see wait_last_update. */
static unsigned last_mk;
static long last_mk_ms;

/* Waits until the last update this app sent has reached the panel.
 * The driver copies an update's pixels from the framebuffer when it gets to
 * it, not when it is sent. An update still waiting when the page changes
 * underneath (a pressed button, then a new app drawing its first page)
 * picks up the new pixels, and the big update that follows collides with
 * it and is dropped, though the driver reports it done. A filmed PW4 showed
 * exactly that: home's pressed Books icon turned into Books' blank page,
 * and Books' library never appeared. So: before a big update right after
 * another one, before running another app, and when an app exits. */
static void wait_last_update(void) {
    if (!last_mk || fbfd < 0) return;
    unsigned m[2] = {last_mk, 0};
    ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, m);
    last_mk = 0;
}

/* full=1 flashes the panel and clears ghosting, full=0 changes only what
 * changed. Drawing can carry on straight away, and wait is almost never
 * needed (but see wait_last_update).
 * An app's first full-screen page is the one that replaces the previous
 * app's screen, and losing it left that screen showing while the app took
 * taps for its own. So that one page is waited for, and if the driver never
 * finishes it, the page is sent again as a full flash. */
static int first_page_pending = 1;
static void refresh(int x, int y, int w, int h, int wf, int full, int wait) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    int big = w * h >= W * H / 2, verify = first_page_pending && big;
    if (verify) first_page_pending = 0;
    /* a big update straight after another (a press, a toast) waits for it,
     * so the two can't collide; 1 s covers the slowest waveform */
    if (big && last_mk && mono_ms() - last_mk_ms < 1000) wait_last_update();
    unsigned mk = send_update(x, y, w, h, wf, full);
    if (mk) { last_mk = mk; last_mk_ms = mono_ms(); }
    if (!mk) {
        /* refused even after retrying: one last try as a plain full update */
        if (verify) send_update(0, 0, W, H, WF_GC16, 1);
        return;
    }
    if (wait || verify) {
        unsigned m[2] = {mk, 0};
        last_mk = 0; /* waited for below, so nothing is left in flight */
        if (ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, m) < 0 && errno == ETIMEDOUT) {
            fprintf(stderr, "%s: screen update %u never finished%s\n", app_name(), mk, verify ? ", redrawing with a flash" : "");
            if (verify) send_update(0, 0, W, H, WF_GC16, 1);
        }
    }
}
/* For an app that ran another one and is about to draw over its screen. */
static void screen_taken_back(void) { first_page_pending = 1; }
static void refresh_all(void) { refresh(0, 0, W, H, WF_GC16, 1, 1); }

/* A screen change. Partial GC16 only redraws pixels that changed, which is
 * quicker and calmer than a flash. Every few pages a full flash clears the
 * ghosting that partial updates leave behind. */
/* The PW4 flashes every 6 pages, the PW3 every 8. */
static int pages_since_flash;
static void flip(void) {
    if (++pages_since_flash >= (board_pw4() ? 6 : 8)) { pages_since_flash = 0; refresh(0, 0, W, H, WF_GC16, 1, 0); }
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
    snapshot s = {x, y, w, h, w > 0 && h > 0 ? malloc((size_t)w * h) : 0};
    for (int j = 0; s.px && j < h; j++) memcpy(s.px + j * w, fb + (y + j) * PITCH + x, w);
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
#ifndef FONT_DIR /* a host build can point this at fonts/ to draw screens off the device */
#define FONT_DIR "/usr/share/fonts/inkwell/"
#endif

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
    g->f = f; g->px = px; g->c = c;
    /* None of the fonts has emoji. Leave them out instead of drawing a box,
     * with the joiners and variation selectors that build them. */
    int emoji = (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0xFE00 && c <= 0xFE0F) || c == 0x200D;
    if (emoji && !stbtt_FindGlyphIndex(&f->info, c)) {
        g->adv = g->w = g->h = 0;
        g->bm = calloc(1, 1);
        return g;
    }
    float sc = stbtt_ScaleForPixelHeight(&f->info, px);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->info, c, &adv, &lsb);
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

/* The power button. The PW3 has one device for it (max77696-onkey). The PW4
 * has two that both claim KEY_POWER, the PMIC's (bd71827-power) and the
 * SoC's (snvs-powerkey), so both are read. */
static int touchfd = -1, keyfd = -1, keyfd2 = -1;
static int tx0, tx1, ty0, ty1;

/* Where the finger last was, kept across apps. The kernel only reports a
 * coordinate that changed, so a tap landing exactly where the previous one
 * lifted sends no position at all. While another app runs, its taps move
 * that position without this app seeing them, so every app writes the last
 * lift to /run/own/touch and a newly started app reads it back. */
#define TOUCH_STATE "/run/own/touch"
static int t_down, t_x, t_y, t_sx, t_sy, t_moved;

static void touch_save(void) {
    FILE *f = fopen(TOUCH_STATE, "w");
    if (f) { fprintf(f, "%d %d\n", t_x, t_y); fclose(f); }
}
static void touch_load(void) {
    FILE *f = fopen(TOUCH_STATE, "r");
    if (f) { if (fscanf(f, "%d %d", &t_x, &t_y) != 2) t_x = t_y = 0; fclose(f); }
}


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
            touch_load();
        } else if ((keyfd < 0 || keyfd2 < 0) && (strstr(name, "onkey") || strstr(name, "power"))) {
            if (keyfd < 0) keyfd = fd; else keyfd2 = fd;
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

/* Idle sleep. ui.h points idle_hook at a function that shows the sleep
 * screen and suspends, so every app sleeps after IDLE_SECS without a touch.
 * Apps that are busy without touches (a terminal printing, Quill writing)
 * call idle_touch() to stay awake. */
#define IDLE_SECS (15 * 60)
static time_t last_input;
static void (*idle_hook)(void);
static void idle_touch(void) { last_input = time(0); }
static void idle_check(void) {
    if (!last_input) idle_touch();
    if (idle_hook && time(0) - last_input >= IDLE_SECS) { idle_hook(); idle_touch(); }
}

static void evq_push(event e) {
    last_input = time(0);
    int next = (evq_tail + 1) % EVQ;
    if (next != evq_head) { evq[evq_tail] = e; evq_tail = next; }
}

static void input_events(int quiet);

/* Drops waiting taps (after a child app, or when the page changes under
 * the finger), but still follows the finger so the position stays right. */
static void input_drain(void) {
    input_events(1);
    t_down = 0;
    evq_head = evq_tail = 0;
}

static void input_read(void) { input_events(0); }

/* 1 when either power key device reports a press. If both report the same
 * press, the second within half a second is dropped. */
static int power_pressed(void) {
    static struct timespec last;
    struct input_event ev[16];
    int fds[2] = {keyfd, keyfd2}, pressed = 0;
    long n;
    for (int k = 0; k < 2; k++)
        while (fds[k] >= 0 && (n = read(fds[k], ev, sizeof ev)) > 0)
            for (int i = 0; i < n / (long)sizeof *ev; i++)
                if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1) pressed = 1;
    if (!pressed) return 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long ms = (t.tv_sec - last.tv_sec) * 1000 + (t.tv_nsec - last.tv_nsec) / 1000000;
    if (last.tv_sec && ms < 500) return 0;
    last = t;
    return 1;
}

static void input_events(int quiet) {
    struct input_event ev[64];
    long n;
    if (power_pressed() && !quiet) evq_push((event){EV_POWER, 0, 0, 0, 0});
    while (touchfd >= 0 && (n = read(touchfd, ev, sizeof ev)) > 0)
        for (int i = 0; i < n / (long)sizeof *ev; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_ABS) {
                if (e->code == ABS_MT_POSITION_X) t_x = (e->value - tx0) * (W - 1) / (tx1 - tx0 ? tx1 - tx0 : 1), t_moved = 1;
                else if (e->code == ABS_MT_POSITION_Y) t_y = (e->value - ty0) * (H - 1) / (ty1 - ty0 ? ty1 - ty0 : 1), t_moved = 1;
                else if (e->code == ABS_MT_TRACKING_ID) {
                    /* The kernel drops coordinates that didn't change, so a finger
                     * landing where the last one lifted sends none. Count the
                     * landing itself as movement. */
                    if (e->value >= 0 && !t_down) { t_down = 1; t_sx = -1; t_moved = 1; }
                    else if (e->value < 0 && t_down) {
                        t_down = 0;
                        touch_save();
                        if (!quiet) evq_push((event){EV_TAP, t_sx >= 0 ? t_sx : t_x, t_sx >= 0 ? t_sy : t_y, t_x, t_y});
                    }
                }
            } else if (e->type == EV_SYN && e->code == SYN_REPORT && t_down && t_moved) {
                t_moved = 0;
                if (t_sx < 0) { t_sx = t_x; t_sy = t_y; if (!quiet) evq_push((event){EV_DOWN, t_x, t_y, t_x, t_y}); }
                else if (want_moves && !quiet) evq_push((event){EV_MOVE, t_x, t_y, t_x, t_y});
            }
        }
}

/* Waits up to timeout_ms (-1 for ever) for the next event. The time left is
 * measured on the clock, so touches that queue nothing (a resting finger, a
 * drag with want_moves off) can't stretch the timeout. */
static event input_wait(int timeout_ms) {
    long deadline = timeout_ms >= 0 ? mono_ms() + timeout_ms : 0;
    /* Waiting for ever is done a minute at a time, so idle sleep can step in. */
    while (evq_head == evq_tail) {
        long left = timeout_ms >= 0 ? deadline - mono_ms() : 60000;
        if (left <= 0) break;
        int step = left > 60000 ? 60000 : (int)left;
        struct pollfd p[3] = {{touchfd, POLLIN, 0}, {keyfd, POLLIN, 0}, {keyfd2, POLLIN, 0}};
        if (poll(p, 3, step) > 0) { input_read(); continue; }
        idle_check();
    }
    if (evq_head == evq_tail) return (event){EV_NONE, 0, 0, 0, 0};
    event e = evq[evq_head];
    evq_head = (evq_head + 1) % EVQ;
    return e;
}

#endif

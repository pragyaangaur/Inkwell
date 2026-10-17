/* kio.h: the Kindle PW3 platform layer shared by the games. Freestanding:
 * raw ARM EABI syscalls, /dev/fb0 plus the Lab126 E-ink refresh ioctl,
 * the cyttsp4 multitouch panel and the power button. Include it once. */
#ifndef KIO_H
#define KIO_H
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

/* ---- syscalls ---------------------------------------------------------- */

static long sys(long n, long a, long b, long c, long d, long e, long f) {
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    register long r7 __asm__("r7") = n;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                     : "memory");
    return r0;
}
#define S_READ 3
#define S_WRITE 4
#define S_OPEN 5
#define S_CLOSE 6
#define S_IOCTL 54
#define S_NANOSLEEP 162
#define S_MMAP2 192
#define S_EXIT 248
#define S_CLOCK 263
#define O_RDWR 2
#define O_NONBLOCK 04000

static long sopen(const char *p, int fl) { return sys(S_OPEN, (long)p, fl, 0, 0, 0, 0); }
static long sread(int fd, void *b, long n) { return sys(S_READ, fd, (long)b, n, 0, 0, 0); }
static long sioctl(int fd, u32 req, void *arg) { return sys(S_IOCTL, fd, (long)req, (long)arg, 0, 0, 0); }
static void say(const char *s) { long n = 0; while (s[n]) n++; sys(S_WRITE, 2, (long)s, n, 0, 0, 0); }

static void logs(const char *s) { say(s); }
static void logn(long v) {
    char b[16]; int i = 15; b[i] = 0;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    say(b + i);
}

static double now(void) {
    s32 ts[2];
    sys(S_CLOCK, 1, (long)ts, 0, 0, 0, 0);
    return ts[0] + ts[1] * 1e-9;
}
static void nap_ms(int ms) {
    s32 ts[2] = {ms / 1000, (ms % 1000) * 1000000};
    sys(S_NANOSLEEP, (long)ts, 0, 0, 0, 0, 0);
}

/* The compiler may emit these, and Cortex-A9 has no hardware divide. */
void *memset(void *d, int c, unsigned n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, unsigned n) {
    u8 *p = d; const u8 *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else while (n--) p[n] = q[n];
    return d;
}
static u32 udivmod(u32 n, u32 d, u32 *rem) {
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    *rem = r;
    return q;
}
u32 __aeabi_uidiv(u32 n, u32 d) { u32 r; return udivmod(n, d, &r); }
s32 __aeabi_idiv(s32 n, s32 d) {
    int neg = (n < 0) ^ (d < 0); u32 r;
    u32 q = udivmod(n < 0 ? -(u32)n : (u32)n, d < 0 ? -(u32)d : (u32)d, &r);
    return neg ? -(s32)q : (s32)q;
}
/* uidivmod / idivmod return the quotient in r0 and the remainder in r1. */
unsigned long long __aeabi_uidivmod(u32 n, u32 d) {
    u32 r, q = udivmod(n, d, &r);
    return ((unsigned long long)r << 32) | q;
}
unsigned long long __aeabi_idivmod(s32 n, s32 d) {
    s32 q = __aeabi_idiv(n, d), r = n - q * d;
    return ((unsigned long long)(u32)r << 32) | (u32)q;
}

/* ---- framebuffer and E-ink refresh ------------------------------------ */

struct mxcfb_rect { u32 top, left, width, height; };
struct mxcfb_update_data {
    struct mxcfb_rect region;
    u32 waveform, mode, marker, hist_bw, hist_gray;
    s32 temp;
    u32 flags;
    u32 alt[7];
};
#define MXCFB_SEND_UPDATE 0x4048462Eu /* _IOW(F, 0x2E, 72 bytes): the PW3 kernel rejects the 76-byte form */
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE 0xC008462Fu
#define WF_DU 1
#define WF_GC16 2
#define WF_A2 4
#define TEMP_AUTO 0x1001
#define FLAG_MONO 0x02

static int fbfd;
static u8 *fb;
static int W, H, PITCH, BPP;
static u32 marker;
static int refresh_errors;

static void refresh(int x, int y, int w, int h, int wf, int full, int wait) {
    struct mxcfb_update_data u;
    memset(&u, 0, sizeof u);
    u.region.top = y; u.region.left = x; u.region.width = w; u.region.height = h;
    u.waveform = wf;
    u.mode = full;
    u.marker = ++marker;
    u.hist_bw = WF_DU;
    u.hist_gray = WF_GC16;
    u.temp = TEMP_AUTO;
    u.flags = wf == WF_A2 ? FLAG_MONO : 0;
    double t0 = now();
    long r = sioctl(fbfd, MXCFB_SEND_UPDATE, &u);
    if (r < 0 && refresh_errors++ < 5) { logs("SEND_UPDATE failed "); logn(r); logs("\n"); }
    if (wait && r >= 0) {
        u32 m[2] = {marker, 0};
        r = sioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, m);
        if (r < 0 && refresh_errors++ < 5) { logs("WAIT_FOR_UPDATE failed "); logn(r); logs("\n"); }
    }
    if (marker <= 5) {
        logs("refresh "); logn(marker); logs(" wf "); logn(wf); logs(" "); logn(w); logs("x"); logn(h);
        logs(" -> "); logn(r); logs(" in "); logn((long)((now() - t0) * 1000)); logs(" ms\n");
    }
}

static void px(int x, int y, u8 g) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    if (BPP == 8) fb[y * PITCH + x] = g;
    else if (BPP == 16) { u16 v = (u16)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3)); ((u16 *)(fb + y * PITCH))[x] = v; }
    else ((u32 *)(fb + y * PITCH))[x] = 0xFF000000u | g << 16 | g << 8 | g;
}
static void fill(int x, int y, int w, int h, u8 g) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, g);
}
static void frame_rect(int x, int y, int w, int h, int t, u8 g) {
    fill(x, y, w, t, g); fill(x, y + h - t, w, t, g);
    fill(x, y, t, h, g); fill(x + w - t, y, t, h, g);
}

/* ---- a 5x7 font for the pad labels ------------------------------------ */

static const char *glyph(char c) {
    static const char *g[] = {
        /* A */ ".###.#...##...#######...##...##...#", /* B */ "####.#...##...#####.#...##...#####.",
        /* C */ ".#####....#....#....#....#.....####", /* D */ "####.#...##...##...##...##...#####.",
        /* E */ "######....#....####.#....#....#####", /* F */ "######....#....####.#....#....#....",
        /* G */ ".#####....#....#..###...##...#.###.", /* H */ "#...##...##...#######...##...##...#",
        /* I */ "#####..#....#....#....#....#..#####", /* J */ "....#....#....#....#....##...#.###.",
        /* K */ "#...##..#.#.#..##...#.#..#..#.#...#", /* L */ "#....#....#....#....#....#....#####",
        /* M */ "#...###.###.#.##...##...##...##...#", /* N */ "#...###..##.#.##..###...##...##...#",
        /* O */ ".###.#...##...##...##...##...#.###.", /* P */ "####.#...##...#####.#....#....#....",
        /* Q */ ".###.#...##...##...##.#.##..#..##.#", /* R */ "####.#...##...#####.#.#..#..#.#...#",
        /* S */ ".#####....#.....###.....#....#####.", /* T */ "#####..#....#....#....#....#....#..",
        /* U */ "#...##...##...##...##...##...#.###.", /* V */ "#...##...##...##...##...#.#.#...#..",
        /* W */ "#...##...##...##.#.##.#.##.#.#.#.#.", /* X */ "#...##...#.#.#...#...#.#.#...##...#",
        /* Y */ "#...##...#.#.#...#....#....#....#..", /* Z */ "#####....#...#...#...#...#....#####",
    };
    static const char *d[] = {
        ".###.#...##..###.#.###..##...#.###.", "..#...##....#....#....#....#...###.",
        ".###.#...#....#...#...#...#...#####", ".###.#...#....#..##.....##...#.###.",
        "...#...##..#.#.#..#######...#....#.", "######....####.....#....##...#.###.",
        ".###.#....#....####.#...##...#.###.", "#####....#...#...#...#....#....#...",
        ".###.#...##...#.###.#...##...#.###.", ".###.#...##...#.####....#....#.###.",
    };
    if (c == '-') return "...............#####...............";
    if (c == '!') return "..#....#....#....#....#.........#..";
    if (c == ':') return ".......##...##.........##...##.....";
    if (c >= 'A' && c <= 'Z') return g[c - 'A'];
    if (c >= '0' && c <= '9') return d[c - '0'];
    return 0;
}
static void text(int x, int y, int s, const char *t, u8 g) {
    for (; *t; t++, x += 6 * s) {
        const char *b = glyph(*t);
        if (!b) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (b[r * 5 + c] == '#') fill(x + c * s, y + r * s, s, s, g);
    }
}
static int text_w(const char *t, int s) { int n = 0; while (t[n]) n++; return n * 6 * s - s; }

/* ---- touch ------------------------------------------------------------ */

#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define ABS_MT_SLOT 0x2f
#define ABS_MT_X 0x35
#define ABS_MT_Y 0x36
#define ABS_MT_ID 0x39
#define KEY_POWER 116
#define EVIOCGABS(a) (0x80184540u + (a))
#define NSLOT 10

typedef struct { s32 tv_sec, tv_usec; u16 type, code; s32 value; } input_event;
typedef struct { s32 value, min, max, fuzz, flat, res; } absinfo;

static int touchfd = -1, keyfd = -1;
static int tx0, tx1, ty0, ty1;
static struct { int id, x, y, px, py, btn, fresh; } slot[NSLOT];
static int cur_slot;
static double last_activity;
#define IDLE_QUIT_S 180.0 /* nobody touched it for 3 minutes: give the Kindle back */

static int has(const char *s, const char *w) {
    for (; *s; s++) { int i = 0; while (w[i] && s[i] == w[i]) i++; if (!w[i]) return 1; }
    return 0;
}

static void open_input(void) {
    char path[] = "/dev/input/eventN", name[64];
    for (int n = 0; n < 8; n++) {
        path[16] = (char)('0' + n);
        int fd = (int)sopen(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        memset(name, 0, sizeof name);
        sioctl(fd, 0x80404506u, name); /* EVIOCGNAME(64) */
        absinfo ax, ay;
        int touch = sioctl(fd, EVIOCGABS(ABS_MT_X), &ax) >= 0 &&
                    sioctl(fd, EVIOCGABS(ABS_MT_Y), &ay) >= 0 && ax.max > 0;
        logs("input "); logs(path); logs(" "); logs(name);
        if (touch && touchfd < 0) {
            touchfd = fd; tx0 = ax.min; tx1 = ax.max; ty0 = ay.min; ty1 = ay.max;
            logs(" touch x "); logn(tx0); logs(".."); logn(tx1);
            logs(" y "); logn(ty0); logs(".."); logn(ty1);
        } else if (keyfd < 0 && has(name, "onkey")) {
            keyfd = fd;
            logs(" power");
        } else {
            sys(S_CLOSE, fd, 0, 0, 0, 0, 0);
        }
        logs("\n");
    }
    for (int i = 0; i < NSLOT; i++) slot[i].id = -1, slot[i].btn = -1;
}

/* Returns 1 when the power button was pressed. */
static int poll_input(void) {
    input_event ev[64];
    int quit = 0;
    long n;
    while (touchfd >= 0 && (n = sread(touchfd, ev, sizeof ev)) > 0) {
        last_activity = now();
        for (int i = 0; i < n / (long)sizeof(input_event); i++) {
            input_event *e = &ev[i];
            if (e->type == EV_ABS) {
                if (e->code == ABS_MT_SLOT && e->value >= 0 && e->value < NSLOT) cur_slot = e->value;
                else if (e->code == ABS_MT_ID) {
                    slot[cur_slot].id = e->value;
                    if (e->value >= 0) slot[cur_slot].fresh = 1;
                } else if (e->code == ABS_MT_X)
                    slot[cur_slot].x = ((e->value - tx0) * (W - 1) / (tx1 - tx0 ? tx1 - tx0 : 1));
                else if (e->code == ABS_MT_Y)
                    slot[cur_slot].y = ((e->value - ty0) * (H - 1) / (ty1 - ty0 ? ty1 - ty0 : 1));
            }
        }
    }
    while (keyfd >= 0 && (n = sread(keyfd, ev, sizeof ev)) > 0)
        for (int i = 0; i < n / (long)sizeof(input_event); i++)
            if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1) { logs("power button\n"); quit = 1; }
    if (last_activity == 0) last_activity = now();
    if (now() - last_activity > IDLE_QUIT_S) { logs("idle, quitting\n"); quit = 1; }
    return quit;
}


/* ---- startup ---------------------------------------------------------- */

static int fb_init(void) {
    fbfd = (int)sopen("/dev/fb0", O_RDWR);
    if (fbfd < 0) { logs("cannot open /dev/fb0\n"); return -1; }
    u32 v[40], f[20];
    sioctl(fbfd, 0x4600, v); /* FBIOGET_VSCREENINFO */
    sioctl(fbfd, 0x4602, f); /* FBIOGET_FSCREENINFO */
    W = (int)v[0]; H = (int)v[1]; BPP = (int)v[6]; PITCH = (int)f[11];
    logs("fb "); logn(W); logs("x"); logn(H); logs(" virt "); logn(v[2]); logs("x"); logn(v[3]);
    logs(" yoff "); logn(v[5]); logs(" bpp "); logn(BPP); logs(" pitch "); logn(PITCH);
    logs(" rotate "); logn(v[34]); logs(" gray "); logn(v[7]); logs("\n");
    u32 len = PITCH * v[3];
    long m = sys(S_MMAP2, 0, len, 3, 1, fbfd, 0); /* PROT_READ|WRITE, MAP_SHARED */
    if (m < 0 && m > -4096) { logs("mmap failed "); logn(m); logs("\n"); return -1; }
    fb = (u8 *)m + v[5] * PITCH; /* yoffset */
    return 0;
}

static int app_main(void);

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile("mov fp, #0\n"
                     "bic sp, sp, #7\n"
                     "bl kmain\n");
}
__attribute__((noreturn, used)) void kmain(void) {
    int r = app_main();
    sys(S_EXIT, r, 0, 0, 0, 0, 0);
    for (;;) {}
}

#endif

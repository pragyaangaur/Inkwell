/* home: Inkwell's home screen. A serif clock and date, the apps listed in
 * /etc/own/apps (one "Label|icon|command" per line) as line-art icons, a
 * frontlight slider, and a night sky drawn on the panel while the device
 * sleeps. An app that exits 10, 11 or 12 (Settings does) asks own-init for
 * the stock Kindle UI, a reboot or power off, so home exits with that code. */
#include "ui.h"
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define IDLE_SLEEP_S (15 * 60)
#define LIGHT "/sys/class/backlight/max77696-bl/"
#define BATTERY "/sys/class/power_supply/max77696-battery/"

typedef struct { char label[32], icon[16], cmd[200]; rect r; } app;
static app apps[12];
static int napps;
static char device_name[64] = "Inkwell";

static int read_int(const char *path) {
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    return v;
}
static void read_str(const char *path, char *out, int n) {
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) { if (fgets(out, n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}
static void write_int(const char *path, int v) {
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", v); fclose(f); }
}

static int ip_of(const char *ifname, char *out) {
    struct ifreq r;
    int s = socket(AF_INET, SOCK_DGRAM, 0), ok = -1;
    memset(&r, 0, sizeof r);
    strncpy(r.ifr_name, ifname, IFNAMSIZ - 1);
    if (s >= 0 && ioctl(s, SIOCGIFADDR, &r) == 0) { strcpy(out, inet_ntoa(((struct sockaddr_in *)&r.ifr_addr)->sin_addr)); ok = 0; }
    if (s >= 0) close(s);
    return ok;
}

static void load_config(void) {
    char line[300];
    read_str("/etc/own/name", device_name, sizeof device_name);
    if (!device_name[0]) strcpy(device_name, "Inkwell");
    FILE *f = fopen("/etc/own/apps", "r");
    while (f && fgets(line, sizeof line, f) && napps < 12) {
        line[strcspn(line, "\n")] = 0;
        char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : 0;
        if (line[0] == '#' || !b) continue;
        *a = *b = 0;
        app *p = &apps[napps++];
        snprintf(p->label, sizeof p->label, "%s", line);
        snprintf(p->icon, sizeof p->icon, "%s", a + 1);
        snprintf(p->cmd, sizeof p->cmd, "%s", b + 1);
    }
    if (f) fclose(f);
}

/* ---- layout ------------------------------------------------------------------- */

#define STATUS_Y 34
#define CLOCK_Y 70
#define DATE_Y 318
#define RULE_Y 440
#define GRID_Y 490
#define CELL_H 250
#define SLIDER_Y 1290
#define SLIDER_H 70

static rect slider_r;
static int slider_x0, slider_x1;

static void layout(void) {
    /* Three columns hold nine apps. More than that goes to four columns,
     * which keeps the rows the same height. */
    int ncol = napps > 9 ? 4 : 3, cw = (W - 2 * MARGIN) / ncol;
    for (int i = 0; i < napps; i++) apps[i].r = (rect){MARGIN + (i % ncol) * cw, GRID_Y + (i / ncol) * CELL_H, cw, CELL_H};
    slider_x0 = MARGIN + 90;
    slider_x1 = W - MARGIN - 130;
    slider_r = (rect){MARGIN, SLIDER_Y - 20, W - 2 * MARGIN, SLIDER_H + 40};
}

/* ---- drawing ------------------------------------------------------------------ */

static void draw_status(void) {
    char buf[64], st[32], ip[32];
    fill(0, STATUS_Y - 10, W, 60, WHITE);
    /* the wordmark, letter-spaced small caps */
    int x = MARGIN;
    for (const char *p = "INKWELL"; *p; p++) {
        char c[2] = {*p, 0};
        x = text(&f_ui_bold, 26, x, STATUS_Y, c, BLACK) + 7;
    }
    int pct = read_int(BATTERY "capacity");
    read_str(BATTERY "status", st, sizeof st);
    snprintf(buf, sizeof buf, "%d%%", pct);
    int rx = W - MARGIN;
    text_right(&f_ui_bold, 28, rx, STATUS_Y - 2, buf, BLACK);
    rx -= text_width(&f_ui_bold, 28, buf) + 70;
    battery_glyph(rx, STATUS_Y + 2, 26, pct, strcmp(st, "Charging") == 0);
    if (ip_of("wlan0", ip) == 0) {
        rx -= 50;
        for (int i = 1; i <= 3; i++) arc(rx, STATUS_Y + 30, 9.0f * i, 4, 3.93f, 5.50f, BLACK);
        disc(rx, STATUS_Y + 29, 3.5f, BLACK);
    }
    if (ip_of("usb0", ip) == 0) {
        rx -= 60;
        text(&f_ui_bold, 22, rx - 24, STATUS_Y + 4, "USB", 0x50);
    }
}

static void draw_clock(void) {
    char hm[16], date[64];
    time_t now = time(0);
    struct tm *tm = localtime(&now);
    strftime(hm, sizeof hm, "%H:%M", tm);
    strftime(date, sizeof date, "%A, %-d %B", tm);
    fill(0, CLOCK_Y, W, RULE_Y - 20 - CLOCK_Y, WHITE);
    text(&f_serif_bold, 290, MARGIN - 8, CLOCK_Y - 20, hm, BLACK);
    text(&f_serif_italic, 62, MARGIN, DATE_Y, date, 0x30);
}

static void draw_app(const app *a) {
    int cx = a->r.x + a->r.w / 2, cy = a->r.y + 92;
    fill(a->r.x, a->r.y, a->r.w, a->r.h, WHITE);
    ring(cx, cy, 78, 3, 0x40);
    icon(a->icon, cx, cy, 128);
    text_center(&f_ui_bold, 32, cx, a->r.y + 186, a->label, BLACK);
}

static int light_level(void) {
    int b = read_int(LIGHT "brightness"), m = read_int(LIGHT "max_brightness");
    if (m <= 0 || b <= 0) return 0;
    /* the slider is quadratic, which spreads out the dim end where eyes are fussy */
    return (int)(sqrtf((float)b / m) * 100 + 0.5f);
}

static void draw_slider(void) {
    int y = SLIDER_Y + SLIDER_H / 2, pct = light_level();
    fill(slider_r.x, slider_r.y, slider_r.w, slider_r.h, WHITE);
    icon_sun(MARGIN + 34, y, 64, 0x40);
    rrect_fill(slider_x0, y - 5, slider_x1 - slider_x0, 10, 5, 0xC0);
    int kx = slider_x0 + (slider_x1 - slider_x0) * pct / 100;
    rrect_fill(slider_x0, y - 5, kx - slider_x0, 10, 5, BLACK);
    disc(kx, y, 24, BLACK);
    disc(kx, y, 13, WHITE);
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", pct);
    text_right(&f_ui_bold, 32, W - MARGIN, y - 22, buf, 0x30);
}

static const char *const footers[] = {
    "ink on glass, linux underneath",
    "write it down before it fades",
    "nothing in here answers to anyone but you",
    "the page holds still. so can you",
    "turn the page",
    "small machine, quiet mind",
};

static void draw_home(int flash) {
    fill(0, 0, W, H, WHITE);
    draw_status();
    draw_clock();
    ornament(RULE_Y);
    for (int i = 0; i < napps; i++) draw_app(&apps[i]);
    draw_slider();
    time_t now = time(0);
    text_center(&f_hand, 46, W / 2, H - 82, footers[(now / 86400) % (sizeof footers / sizeof *footers)], 0x50);
    if (flash) flip_flash(); else flip();
}

/* ---- the sleep screen ------------------------------------------------------------- */

static unsigned rng_state = 12345;
static unsigned rng(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

/* Grey with ordered dithering between the panel's 16 levels, so gradients
 * come out smooth instead of banded. */
static inline unsigned char shade(int x, int y, int v) {
    int t = bayer8[(y & 7) * 8 + (x & 7)] * 4, l = (v * 15 + t) / 255;
    return (unsigned char)((l > 15 ? 15 : l) * 17);
}

static void draw_night(void) {
    int horizon = H - 330;
    for (int y = 0; y < H; y++) {
        int v = 18 + 70 * y / H;
        unsigned char *row = fb + y * PITCH;
        for (int x = 0; x < W; x++) row[x] = shade(x, y, v);
    }
    rng_state = (unsigned)time(0) / 3600;
    for (int i = 0; i < 160; i++) {
        float x = rng() % W, y = rng() % horizon, r = 1.2f + (rng() % 100) / 40.0f;
        disc(x, y, r, 0xF0);
        if (r > 3.2f) { line(x - r * 3, y, x + r * 3, y, 1.4f, 0xC0); line(x, y - r * 3, x, y + r * 3, 1.4f, 0xC0); }
    }
    /* a crescent moon: the lit disc minus a slightly offset shadow disc */
    float mx = W * 0.68f, my = 330, mr = 140, sx = mx + 58, sy = my - 40, sr = 128;
    for (int y = (int)(my - mr - 2); y <= (int)(my + mr + 2); y++)
        for (int x = (int)(mx - mr - 2); x <= (int)(mx + mr + 2); x++) {
            float lit = cover(hypotf(x + 0.5f - mx, y + 0.5f - my) - mr);
            float dark = cover(hypotf(x + 0.5f - sx, y + 0.5f - sy) - sr);
            blend(x, y, 0xF4, lit * (1 - dark));
        }
    /* two ranges of hills */
    float back[2 * 66], front[2 * 66];
    for (int i = 0; i <= 63; i++) {
        float x = W * i / 63.0f;
        back[2 * i] = x; back[2 * i + 1] = horizon + 40 - 70 * sinf(x / 170.0f + 1) - 40 * sinf(x / 61.0f);
        front[2 * i] = x; front[2 * i + 1] = horizon + 150 - 50 * sinf(x / 230.0f + 3) - 22 * sinf(x / 47.0f);
    }
    back[128] = W; back[129] = H; back[130] = 0; back[131] = H;
    front[128] = W; front[129] = H; front[130] = 0; front[131] = H;
    poly_fill(back, 66, 0x30);
    poly_fill(front, 66, BLACK);
    text_center(&f_serif_italic, 78, W / 2, H - 250, "Inkwell is asleep", 0xF0);
    text_center(&f_ui, 32, W / 2, H - 140, "press the power button to wake", 0xB0);
}

static void sleep_device(void) {
    int light = read_int(LIGHT "brightness");
    draw_night();
    refresh_all();
    write_int(LIGHT "brightness", 0);
    sync();
    FILE *f = fopen("/sys/power/state", "w");
    if (f) { fputs("mem\n", f); fclose(f); } /* returns once the power button wakes us */
    write_int(LIGHT "brightness", light);
    input_drain();
}

/* ---- actions ----------------------------------------------------------------------- */

static int run_app(const app *a) {
    int st = 0;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", a->cmd, (char *)0);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, &st, 0);
    input_drain();
    return WIFEXITED(st) ? WEXITSTATUS(st) : 0;
}

static void set_light_from(int x) {
    int m = read_int(LIGHT "max_brightness");
    if (m <= 0) return;
    float f = (float)(x - slider_x0) / (slider_x1 - slider_x0);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    int b = (int)(f * f * m + 0.5f);
    write_int(LIGHT "brightness", b);
    write_int("/mnt/us/linux/light", b); /* own-init restores it at boot */
}

int main(int argc, char **argv) {
    signal(SIGCHLD, SIG_DFL);
    if (fb_open() || fonts_load()) return 1;
    /* home --sleep-preview draws the sleep screen without suspending */
    if (argc > 1 && strcmp(argv[1], "--sleep-preview") == 0) { draw_night(); refresh_all(); return 0; }
    input_open();
    load_config();
    layout();
    draw_home(1);
    time_t last_touch = time(0);
    for (;;) {
        time_t now = time(0);
        event e = input_wait((60 - now % 60) * 1000);
        now = time(0);
        if (e.type == EV_NONE) {
            if (now - last_touch > IDLE_SLEEP_S) { sleep_device(); last_touch = time(0); draw_home(1); continue; }
            draw_status();
            draw_clock();
            refresh(0, 0, W, RULE_Y - 20, WF_GL16, 0, 0);
            continue;
        }
        last_touch = now;
        if (e.type == EV_POWER) { sleep_device(); last_touch = time(0); draw_home(1); continue; }
        if (e.type == EV_DOWN) {
            if (hit(slider_r, e.x, e.y)) { set_light_from(e.x); draw_slider(); refresh(slider_r.x, slider_r.y, slider_r.w, slider_r.h, WF_A2, 0, 0); continue; }
            for (int i = 0; i < napps; i++)
                if (hit(apps[i].r, e.x, e.y)) {
                    int cx = apps[i].r.x + apps[i].r.w / 2;
                    press_on((rect){cx - 84, apps[i].r.y + 6, 168, 172});
                }
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(slider_r, e.x, e.y)) {
            set_light_from(e.ux);
            draw_slider();
            refresh(slider_r.x, slider_r.y, slider_r.w, slider_r.h, WF_GL16, 0, 0);
            continue;
        }
        int launched = 0;
        for (int i = 0; i < napps; i++) {
            if (!hit(apps[i].r, e.x, e.y)) continue;
            press_forget();
            launched = 1;
            int rc = run_app(&apps[i]);
            if (rc >= 10 && rc <= 12) return rc;
            draw_home(0);
            last_touch = time(0);
            break;
        }
        if (!launched) press_off();
    }
}

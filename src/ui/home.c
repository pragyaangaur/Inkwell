/* home: Inkwell's home screen. A serif clock and date, the apps listed in
 * /etc/own/apps (one "Label|icon|command" per line) as line-art icons, a
 * frontlight slider. The sleep screen lives in sleep.h. An app that exits
 * 10, 11 or 12 (Settings does) asks own-init for the stock Kindle UI, a
 * reboot or power off, so home exits with that code. Dragging an app onto
 * another place moves it there, and the order is saved to /etc/own/apps. */
#include "ui.h"
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define LIGHT "/sys/class/backlight/max77696-bl/"
#define BATTERY "/sys/class/power_supply/max77696-battery/"

typedef struct { char label[32], icon[16], cmd[200]; rect r; } app;
static app apps[16];
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
    while (f && fgets(line, sizeof line, f) && napps < 16) {
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

/* Writes the apps back in their current order, keeping the comment lines. */
static void save_config(void) {
    char line[300];
    FILE *in = fopen("/etc/own/apps", "r"), *out = fopen("/etc/own/apps.new", "w");
    if (!out) { if (in) fclose(in); return; }
    while (in && fgets(line, sizeof line, in)) if (line[0] == '#') fputs(line, out);
    if (in) fclose(in);
    for (int i = 0; i < napps; i++) fprintf(out, "%s|%s|%s\n", apps[i].label, apps[i].icon, apps[i].cmd);
    fclose(out);
    rename("/etc/own/apps.new", "/etc/own/apps");
}

/* ---- layout ------------------------------------------------------------------- */

#define STATUS_Y 34
#define CLOCK_Y 70
#define DATE_Y 318
#define RULE_Y 440
#define GRID_Y 490
#define CELL_H 250
#define GRID_BOTTOM 1262
static int cell_h = CELL_H;
#define SLIDER_Y 1290
#define SLIDER_H 70

static rect slider_r;
static int slider_x0, slider_x1;

static void layout(void) {
    /* Three columns hold nine apps. More than that goes to four columns,
     * which keeps the rows the same height. */
    int ncol = napps > 9 ? 4 : 3, cw = (W - 2 * MARGIN) / ncol;
    /* A fourth row makes the cells shorter, and draw_app scales to them. */
    int nrow = (napps + ncol - 1) / ncol;
    cell_h = nrow * CELL_H > GRID_BOTTOM - GRID_Y ? (GRID_BOTTOM - GRID_Y) / nrow : CELL_H;
    for (int i = 0; i < napps; i++) apps[i].r = (rect){MARGIN + (i % ncol) * cw, GRID_Y + (i / ncol) * cell_h, cw, cell_h};
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

static int app_at(int x, int y) {
    for (int i = 0; i < napps; i++) if (hit(apps[i].r, x, y)) return i;
    return -1;
}

/* A dark frame on the place an app is being dragged to. */
static void drop_mark(int i, int on) {
    rect r = apps[i].r;
    if (on) { frame(r.x + 10, r.y + 4, r.w - 20, r.h - 8, 5, BLACK); refresh(r.x, r.y, r.w, r.h, WF_A2, 0, 0); }
}

static void draw_app(const app *a) {
    int cx = a->r.x + a->r.w / 2, cy = a->r.y + 92;
    fill(a->r.x, a->r.y, a->r.w, a->r.h, WHITE);
    float k = cell_h / (float)CELL_H;
    cy = a->r.y + (int)(92 * k);
    ring(cx, cy, 78 * k, 3, 0x40);
    icon(a->icon, cx, cy, 128 * k);
    text_center(&f_ui_bold, k < 1 ? 28 : 32, cx, a->r.y + (int)(186 * k) - (k < 1 ? 4 : 0), a->label, BLACK);
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
    want_moves = 1; /* to follow an app being dragged */
    int drag = -1, over = -1;
    for (;;) {
        time_t now = time(0);
        event e = input_wait((60 - now % 60) * 1000);
        now = time(0);
        if (e.type == EV_NONE) {
            draw_status();
            draw_clock();
            refresh(0, 0, W, RULE_Y - 20, WF_GL16, 0, 0);
            continue;
        }
        if (e.type == EV_POWER) { sleep_now(); draw_home(1); continue; }
        if (e.type == EV_MOVE) {
            int j = drag >= 0 ? app_at(e.x, e.y) : -1;
            if (j != over) {
                if (over >= 0 && over != drag) { draw_app(&apps[over]); refresh(apps[over].r.x, apps[over].r.y, apps[over].r.w, apps[over].r.h, WF_GL16, 0, 0); }
                over = j;
                if (over >= 0 && over != drag) drop_mark(over, 1);
            }
            continue;
        }
        if (e.type == EV_DOWN) {
            drag = over = -1;
            if (hit(slider_r, e.x, e.y)) { set_light_from(e.x); draw_slider(); refresh(slider_r.x, slider_r.y, slider_r.w, slider_r.h, WF_A2, 0, 0); continue; }
            drag = app_at(e.x, e.y);
            for (int i = 0; i < napps; i++)
                if (hit(apps[i].r, e.x, e.y)) {
                    int cx = apps[i].r.x + apps[i].r.w / 2;
                    int r = (int)(84 * cell_h / (float)CELL_H);
                    press_on((rect){cx - r, apps[i].r.y + 6, 2 * r, (int)(172 * cell_h / (float)CELL_H)});
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
        /* lifted on another place, well away from where it landed: a move */
        int to = app_at(e.ux, e.uy), from = drag;
        drag = over = -1;
        if (from >= 0 && to >= 0 && to != from && abs(e.ux - e.x) + abs(e.uy - e.y) > 80) {
            press_forget();
            app moved = apps[from];
            if (from < to) memmove(&apps[from], &apps[from + 1], sizeof *apps * (to - from));
            else memmove(&apps[to + 1], &apps[to], sizeof *apps * (from - to));
            apps[to] = moved;
            layout();
            save_config();
            draw_home(0);
            continue;
        }
        if (from >= 0 && to != from) { press_off(); draw_home(0); continue; } /* dragged off the grid: nothing */
        int launched = 0;
        for (int i = 0; i < napps; i++) {
            if (!hit(apps[i].r, e.x, e.y)) continue;
            press_forget();
            launched = 1;
            int rc = run_app(&apps[i]);
            if (rc >= 10 && rc <= 12) return rc;
            draw_home(0);
            break;
        }
        if (!launched) press_off();
    }
}

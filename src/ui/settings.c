/* settings: restart, power off, the stock Kindle UI, a graph of the battery
 * over the last day (own-init logs it every ten minutes), and an About card
 * with what the device is doing. Exits 10, 11 or 12 to ask home (and through it
 * own-init) for the Kindle UI, a reboot or power off. */
#include "ui.h"
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>

#define BATTERY "/sys/class/power_supply/max77696-battery/"
#define BATTERY_LOG "/mnt/us/linux/battery.log"
#define DAY (24 * 3600)

static void read_str(const char *path, char *out, int n) {
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) { if (fgets(out, n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

static rect restart_btn, off_btn, kindle_btn;

static void about_row(int *y, const char *k, const char *v) {
    char fit[120];
    text(&f_ui, 30, MARGIN + 30, *y, k, 0x60);
    text_fit(&f_ui_bold, 30, v, W - 2 * MARGIN - 330, fit, sizeof fit);
    text_right(&f_ui_bold, 30, W - MARGIN - 30, *y, fit, BLACK);
    *y += 50;
}

/* The battery over the last 24 hours, with charging shaded. */
static void battery_graph(int x, int y, int w, int h) {
    static long t[2000];
    static int pct[2000], chg[2000];
    int n = 0;
    long now = (long)time(0);
    char st[32];
    FILE *f = fopen(BATTERY_LOG, "r");
    while (f && n < 2000 && fscanf(f, "%ld %d %31s", &t[n], &pct[n], st) == 3) {
        chg[n] = !strcmp(st, "Charging") || !strcmp(st, "Full");
        if (t[n] > now - DAY && t[n] <= now) n++;
    }
    if (f) fclose(f);
    rrect_line(x, y, w, h, 18, 3, BLACK);
    int gx = x + 70, gw = w - 100, gy = y + 24, gh = h - 74;
    for (int k = 0; k <= 4; k++) {
        int yy = gy + gh - gh * k / 4;
        fill(gx, yy, gw, 1, k == 0 ? 0x60 : 0xC8);
        if (k % 2 == 0) {
            char l[8];
            snprintf(l, sizeof l, "%d", k * 25);
            text_right(&f_ui, 22, gx - 10, yy - 13, l, 0x60);
        }
    }
    static const char *ticks[] = {"24h ago", "18h", "12h", "6h", "now"};
    for (int k = 0; k <= 4; k++) {
        int xx = gx + gw * k / 4;
        fill(xx, gy + gh, 1, 8, 0x60);
        int tw = text_width(&f_ui, 22, ticks[k]);
        text(&f_ui, 22, k == 0 ? xx : k == 4 ? xx - tw : xx - tw / 2, gy + gh + 12, ticks[k], 0x60);
    }
    if (n < 2) {
        const char *msg = "The graph fills in over the next few hours.";
        int mw = text_width(&f_ui, 28, msg);
        fill(gx + gw / 2 - mw / 2 - 16, gy + gh / 2 - 26, mw + 32, 56, WHITE);
        text_center(&f_ui, 28, gx + gw / 2, gy + gh / 2 - 16, msg, 0x60);
        return;
    }
    for (int i = 1; i < n; i++) {
        float x0 = gx + gw * (float)(t[i - 1] - (now - DAY)) / DAY, x1 = gx + gw * (float)(t[i] - (now - DAY)) / DAY;
        float y0 = gy + gh - gh * pct[i - 1] / 100.0f, y1 = gy + gh - gh * pct[i] / 100.0f;
        if (t[i] - t[i - 1] > 1800) continue; /* the Kindle was off or asleep: leave a gap */
        if (chg[i]) fill((int)x0, (int)fminf(y0, y1), (int)(x1 - x0) + 1, gy + gh - (int)fminf(y0, y1), 0xD8);
        line(x0, y0, x1, y1, 4, BLACK);
    }
}

static void draw(void) {
    char buf[160], a[64], b[64];
    header("Settings");
    int bw = (W - 2 * MARGIN - 40) / 2;
    restart_btn = (rect){MARGIN, 210, bw, 110};
    off_btn = (rect){MARGIN + bw + 40, 210, bw, 110};
    kindle_btn = (rect){MARGIN, 350, W - 2 * MARGIN, 110};
    pill(restart_btn, "Restart", 0);
    pill(off_btn, "Power off", 0);
    pill(kindle_btn, "Open the stock Kindle UI once", 0);

    text(&f_serif_bold, 52, MARGIN, 490, "Battery", BLACK);
    battery_graph(MARGIN, 566, W - 2 * MARGIN, 270);
    int y = 870;
    text(&f_serif_bold, 52, MARGIN, y, "About this Kindle", BLACK);
    y += 90;
    rrect_line(MARGIN, y - 20, W - 2 * MARGIN, 50 * 8 + 24, 24, 3, BLACK);
    struct utsname u;
    uname(&u);
    read_str("/etc/alpine-release", a, sizeof a);
    snprintf(buf, sizeof buf, "Inkwell on Alpine %s", a);
    about_row(&y, "System", buf);
    about_row(&y, "Kernel", u.release);
    struct sysinfo si;
    sysinfo(&si);
    snprintf(buf, sizeof buf, "%lu of %lu MB used", (si.totalram - si.freeram - si.bufferram) * si.mem_unit >> 20, si.totalram * si.mem_unit >> 20);
    about_row(&y, "Memory", buf);
    struct statvfs vs;
    if (statvfs("/mnt/us", &vs) == 0) {
        snprintf(buf, sizeof buf, "%llu MB free", (unsigned long long)vs.f_bavail * vs.f_frsize >> 20);
        about_row(&y, "Storage", buf);
    }
    snprintf(buf, sizeof buf, "%ldh %02ldm", si.uptime / 3600, si.uptime / 60 % 60);
    about_row(&y, "Up for", buf);
    read_str(BATTERY "capacity", a, sizeof a);
    read_str(BATTERY "status", b, sizeof b);
    snprintf(buf, sizeof buf, "%s%%, %s", a, b);
    about_row(&y, "Battery", buf);
    read_str(BATTERY "cycle_count", a, sizeof a);
    snprintf(buf, sizeof buf, "%s cycles", a[0] ? a : "?");
    about_row(&y, "Battery age", buf);
    read_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", a, sizeof a);
    snprintf(buf, sizeof buf, "i.MX6 SoloLite, %s", a);
    about_row(&y, "Processor", buf);

    text_center(&f_hand, 40, W / 2, H - 66, "everything on this screen is yours", 0x60);
    flip();
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    draw();
    for (;;) {
        event e = input_wait(-1);
        if (e.type == EV_POWER) return 0;
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(restart_btn, e.x, e.y)) press_on(restart_btn);
            else if (hit(off_btn, e.x, e.y)) press_on(off_btn);
            else if (hit(kindle_btn, e.x, e.y)) press_on(kindle_btn);
            continue;
        }
        if (e.type != EV_TAP) continue;
        press_forget();
        if (hit(back_btn, e.x, e.y)) return 0;
        if (hit(restart_btn, e.x, e.y)) return 11;
        if (hit(off_btn, e.x, e.y)) return 12;
        if (hit(kindle_btn, e.x, e.y)) return 10;
        press_off();
    }
}

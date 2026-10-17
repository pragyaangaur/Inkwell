/* settings: restart, power off, the stock Kindle UI, and an About card with
 * what the device is doing. Exits 10, 11 or 12 to ask home (and through it
 * own-init) for the Kindle UI, a reboot or power off. */
#include "ui.h"
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#define BATTERY "/sys/class/power_supply/max77696-battery/"

static void read_str(const char *path, char *out, int n) {
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) { if (fgets(out, n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

static rect restart_btn, off_btn, kindle_btn;

static void about_row(int *y, const char *k, const char *v) {
    char fit[120];
    text(&f_ui, 34, MARGIN + 30, *y, k, 0x60);
    text_fit(&f_ui_bold, 34, v, W - 2 * MARGIN - 330, fit, sizeof fit);
    text_right(&f_ui_bold, 34, W - MARGIN - 30, *y, fit, BLACK);
    *y += 62;
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

    int y = 540;
    text(&f_serif_bold, 60, MARGIN, y, "About this Kindle", BLACK);
    y += 100;
    rrect_line(MARGIN, y - 20, W - 2 * MARGIN, 62 * 9 + 30, 24, 3, BLACK);
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
    about_row(&y, "Display", "1072 x 1448, 300 dpi");

    text_center(&f_hand, 44, W / 2, H - 110, "everything on this screen is yours", 0x60);
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

/* board.h: what differs between the Kindles Inkwell runs on. The Paperwhite 3
 * (i.MX6SL, kernel 3.0, MAX77696 PMIC) and the Paperwhite 4 (i.MX6SLL "rex",
 * kernel 4.1, BD71827 PMIC) have the same 1072x1448 panel, but not the same
 * EPDC update ioctl, waveform numbers, frontlight or battery driver.
 * Included by eink.h. */
#ifndef BOARD_H
#define BOARD_H
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The PW3 kernel has no device tree. The PW4 says "amzn,imx6sll-rex". */
static int board_is_pw4 = -1;
static int board_pw4(void) {
    if (board_is_pw4 >= 0) return board_is_pw4;
    board_is_pw4 = 0;
    FILE *f = fopen("/proc/device-tree/compatible", "r");
    if (f) {
        char b[256];
        size_t n = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        for (size_t i = 0; i < n; i++) if (!b[i]) b[i] = ' '; /* the list is NUL-separated */
        b[n] = 0;
        board_is_pw4 = strstr(b, "imx6sll") != 0;
    }
    return board_is_pw4;
}
static const char *board_name(void) { return board_pw4() ? "Kindle Paperwhite 4" : "Kindle Paperwhite 3"; }
static const char *board_cpu(void) { return board_pw4() ? "i.MX6 SoloLite Lite" : "i.MX6 SoloLite"; }

/* The first directory that exists, so one image works on both. */
static const char *board_dir(const char *const *dirs) {
    for (int i = 0; dirs[i]; i++) if (access(dirs[i], F_OK) == 0) return dirs[i];
    return dirs[0];
}
static const char *light_dir(void) {
    static const char *const d[] = {"/sys/class/backlight/max77696-bl/", "/sys/class/backlight/bl/", 0};
    return board_dir(d);
}
static const char *battery_dir(void) {
    static const char *const d[] = {"/sys/class/power_supply/max77696-battery/", "/sys/class/power_supply/bd71827_bat/", 0};
    return board_dir(d);
}

/* dir + name, in one of a few rotating buffers, so several can be used in
 * one expression. */
static const char *board_path(const char *dir, const char *name) {
    static char buf[4][96];
    static int k;
    char *p = buf[k++ & 3];
    snprintf(p, sizeof buf[0], "%s%s", dir, name);
    return p;
}
#define LIGHT_FILE(name) board_path(light_dir(), name)
#define BATTERY_FILE(name) board_path(battery_dir(), name)

static void board_read(const char *path, char *out, int n) {
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) { if (fgets(out, n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

/* "Charging", "Full" or "Discharging". The BD71827 has no status file, only
 * a charging flag and whether the charger is plugged in. */
static void battery_status(char *out, int n) {
    board_read(BATTERY_FILE("status"), out, n);
    if (out[0]) return;
    char chg[8], ac[8], cap[8];
    board_read(BATTERY_FILE("charging"), chg, sizeof chg);
    board_read("/sys/class/power_supply/bd71827_ac/online", ac, sizeof ac);
    board_read(BATTERY_FILE("capacity"), cap, sizeof cap);
    if (chg[0] == '1') snprintf(out, n, "Charging");
    else if (ac[0] == '1' && cap[0] && strcmp(cap, "100") == 0) snprintf(out, n, "Full");
    else snprintf(out, n, "Discharging");
}

static void battery_cycles(char *out, int n) {
    board_read(BATTERY_FILE("cycle_count"), out, n);
    if (!out[0]) board_read(BATTERY_FILE("battery_cycle"), out, n);
}

#endif

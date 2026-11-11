/* sleep.h: the sleep screen and suspend, shared by every app. Included by
 * ui.h, which also makes it the idle hook, so any app goes to sleep after
 * IDLE_SECS without a touch and wakes with its page as it was. */
#ifndef SLEEP_H
#define SLEEP_H
#include "eink.h"
#include "gfx.h"
#include <math.h>
#include <sys/wait.h>

#define SLEEP_LIGHT LIGHT_FILE("brightness")
/* The SSH app's flags: SSH over Wi-Fi is on, and should keep working while
 * the Kindle sleeps. A suspended Kindle can't answer, so it then only looks
 * asleep. */
#define SSH_WIFI_FLAG "/mnt/us/linux/ssh-wifi"
#define SSH_AWAKE_FLAG "/mnt/us/linux/ssh-wifi.awake"

static unsigned night_seed = 12345;
static unsigned night_rng(void) { night_seed = night_seed * 1103515245u + 12345u; return night_seed >> 8; }

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
    night_seed = (unsigned)time(0) / 3600;
    for (int i = 0; i < 160; i++) {
        float x = night_rng() % W, y = night_rng() % horizon, r = 1.2f + (night_rng() % 100) / 40.0f;
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

/* With sleep_screen = yes in linux/dashboard.conf, the Dashboard is the
 * sleep screen. It draws itself into the page (the framebuffer is shared)
 * and exits 0; anything else, and the night sky is drawn instead. */
#define DASHBOARD_CONF "/mnt/us/linux/dashboard.conf"
static int draw_dashboard_sleep(void) {
    if (access(DASHBOARD_CONF, F_OK)) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/local/bin/dashboard", "dashboard", "--sleep", (char *)0);
        _exit(127);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid) return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int stay_awake(void) {
    return (keyfd >= 0 || keyfd2 >= 0) && access(SSH_WIFI_FLAG, F_OK) == 0 && access(SSH_AWAKE_FLAG, F_OK) == 0;
}

/* Blocks until the power button is pressed, leaving touches unread. */
static void wait_for_power(void) {
    for (;;) {
        struct pollfd p[2] = {{keyfd, POLLIN, 0}, {keyfd2, POLLIN, 0}};
        if (poll(p, 2, -1) > 0 && power_pressed()) return;
    }
}

static void sleep_now(void) {
    snapshot page = snap(0, 0, W, H);
    int awake = stay_awake();
    if (!draw_dashboard_sleep()) draw_night();
    if (awake) text_center(&f_ui, 28, W / 2, H - 86, "SSH over Wi-Fi stays on", 0x90);
    refresh_all();
    int light = -1;
    FILE *f = fopen(SLEEP_LIGHT, "r");
    if (f) { if (fscanf(f, "%d", &light) != 1) light = -1; fclose(f); }
    if ((f = fopen(SLEEP_LIGHT, "w"))) { fputs("0\n", f); fclose(f); }
    sync();
    int keys = keyfd >= 0 || keyfd2 >= 0;
    if (awake) wait_for_power();
    else {
        /* Returns once the power button wakes us. The kernel can refuse
         * (a driver busy, or the USB cable in), and the write then fails
         * at once: stay on the sleep screen until the button instead of
         * waking straight back up. */
        int ok = (f = fopen("/sys/power/state", "w")) != 0;
        if (ok) { ok = fputs("mem\n", f) >= 0; ok = (fclose(f) == 0) && ok; }
        if (!ok && keys) wait_for_power();
    }
    if (light >= 0 && (f = fopen(SLEEP_LIGHT, "w"))) { fprintf(f, "%d\n", light); fclose(f); }
    unsnap(&page);
    refresh_all();
    input_drain();
    idle_touch();
}

__attribute__((constructor)) static void sleep_install(void) { idle_hook = sleep_now; }

#endif

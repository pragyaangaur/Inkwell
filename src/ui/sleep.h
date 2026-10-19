/* sleep.h: the sleep screen and suspend, shared by every app. Included by
 * ui.h, which also makes it the idle hook, so any app goes to sleep after
 * IDLE_SECS without a touch and wakes with its page as it was. */
#ifndef SLEEP_H
#define SLEEP_H
#include "eink.h"
#include "gfx.h"
#include <math.h>

#define SLEEP_LIGHT "/sys/class/backlight/max77696-bl/brightness"

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

static void sleep_now(void) {
    snapshot page = snap(0, 0, W, H);
    draw_night();
    refresh_all();
    int light = -1;
    FILE *f = fopen(SLEEP_LIGHT, "r");
    if (f) { if (fscanf(f, "%d", &light) != 1) light = -1; fclose(f); }
    if ((f = fopen(SLEEP_LIGHT, "w"))) { fputs("0\n", f); fclose(f); }
    sync();
    if ((f = fopen("/sys/power/state", "w"))) { fputs("mem\n", f); fclose(f); } /* returns once the power button wakes us */
    if (light >= 0 && (f = fopen(SLEEP_LIGHT, "w"))) { fprintf(f, "%d\n", light); fclose(f); }
    unsnap(&page);
    refresh_all();
    input_drain();
    idle_touch();
}

__attribute__((constructor)) static void sleep_install(void) { idle_hook = sleep_now; }

#endif

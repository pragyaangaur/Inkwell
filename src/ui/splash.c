/* splash: Inkwell's boot and shutdown screen, an ink drop over the wordmark.
 * With -w N it then waits up to N seconds for the power button and exits 1 if
 * it was pressed, 0 if not.
 * Usage: splash [-w seconds] ["line under the wordmark" ["small line"]] */
#include "ui.h"
#include <time.h>

int main(int argc, char **argv) {
    int wait_s = 0, a = 1;
    if (a + 1 < argc && strcmp(argv[a], "-w") == 0) { wait_s = atoi(argv[a + 1]); a += 2; }
    if (fb_open() || fonts_load()) return 2;
    fill(0, 0, W, H, WHITE);
    icon_drop(W / 2, H / 2 - 230, 300);
    text_center(&f_serif_bold, 150, W / 2, H / 2 - 40, "Inkwell", BLACK);
    if (a < argc) text_center(&f_serif_italic, 56, W / 2, H / 2 + 150, argv[a++], 0x40);
    if (a < argc) text_center(&f_ui, 32, W / 2, H - 160, argv[a++], 0x70);
    refresh(0, 0, W, H, WF_GC16, 1, 0);
    if (!wait_s) return 0;
    input_open();
    /* already holding the power button counts too, so the wait can be short */
    unsigned char keys[KEY_MAX / 8 + 1];
    memset(keys, 0, sizeof keys);
    if (keyfd >= 0 && ioctl(keyfd, EVIOCGKEY(sizeof keys), keys) >= 0 && (keys[KEY_POWER / 8] & (1 << (KEY_POWER % 8)))) return 1;
    input_drain();
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = wait_s * 1000 - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) break;
        event e = input_wait(left);
        if (e.type == EV_POWER) return 1;
    }
    return 0;
}

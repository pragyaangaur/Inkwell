/* Times full-screen and small partial updates for each EPDC waveform mode. */
#include <time.h>
#include "../ui/eink.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void) {
    if (fb_open()) return 1;
    int modes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    for (int i = 0; i < 10; i++) {
        int m = modes[i];
        for (int pass = 0; pass < 2; pass++) {
            int w = pass ? 300 : W, h = pass ? 120 : H;
            fill(0, 0, w, h, (i + pass) & 1 ? 0x00 : 0xFF);
            struct mxcfb_update_data u; memset(&u, 0, sizeof u);
            u.region = (struct mxcfb_rect){0, 0, w, h}; u.waveform = m; u.mode = 0; u.marker = ++marker; u.temp = TEMP_AMBIENT;
            double t0 = now();
            int r = ioctl(fbfd, MXCFB_SEND_UPDATE, &u);
            unsigned mk[2] = {marker, 0};
            if (r == 0) ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, mk);
            printf("wf %2d %s r=%d %4.0f ms\n", m, pass ? "small" : "full ", r, (now() - t0) * 1000);
        }
    }
    fill(0, 0, W, H, 0xFF); refresh(0, 0, W, H, 2, 1, 1);
    return 0;
}

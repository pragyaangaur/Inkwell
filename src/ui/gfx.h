/* gfx.h: anti-aliased drawing on the 8-bit page, for icons and ornaments.
 * Every shape is drawn from a signed distance, so edges get one pixel of
 * soft coverage and look smooth on the 300 dpi panel. Include after eink.h. */
#ifndef GFX_H
#define GFX_H
#include <math.h>

/* Blends ink into one pixel with coverage a in [0, 1]. */
static inline void blend(int x, int y, unsigned char ink, float a) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || a <= 0) return;
    unsigned char *d = &fb[y * PITCH + x];
    if (a >= 1) { *d = ink; return; }
    *d = (unsigned char)(*d + (ink - *d) * a);
}
static inline float cover(float d) { return d < -0.5f ? 1 : d > 0.5f ? 0 : 0.5f - d; }

static void disc(float cx, float cy, float r, unsigned char ink) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++)
            blend(x, y, ink, cover(hypotf(x + 0.5f - cx, y + 0.5f - cy) - r));
}

static void ring(float cx, float cy, float r, float t, unsigned char ink) {
    for (int y = (int)(cy - r - t); y <= (int)(cy + r + t); y++)
        for (int x = (int)(cx - r - t); x <= (int)(cx + r + t); x++)
            blend(x, y, ink, cover(fabsf(hypotf(x + 0.5f - cx, y + 0.5f - cy) - r) - t / 2));
}

/* An arc of a ring from angle a0 to a1 (radians, 0 is right, clockwise on screen). */
static void arc(float cx, float cy, float r, float t, float a0, float a1, unsigned char ink) {
    for (int y = (int)(cy - r - t); y <= (int)(cy + r + t); y++)
        for (int x = (int)(cx - r - t); x <= (int)(cx + r + t); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, a = atan2f(dy, dx);
            while (a < a0) a += 6.2831853f;
            if (a > a1) continue;
            blend(x, y, ink, cover(fabsf(hypotf(dx, dy) - r) - t / 2));
        }
}

/* A line segment with round caps, t pixels thick. */
static void line(float x0, float y0, float x1, float y1, float t, unsigned char ink) {
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy, r = t / 2;
    int xa = (int)(fminf(x0, x1) - r - 1), xb = (int)(fmaxf(x0, x1) + r + 1);
    int ya = (int)(fminf(y0, y1) - r - 1), yb = (int)(fmaxf(y0, y1) + r + 1);
    for (int y = ya; y <= yb; y++)
        for (int x = xa; x <= xb; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float h = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            h = h < 0 ? 0 : h > 1 ? 1 : h;
            blend(x, y, ink, cover(hypotf(px - vx * h, py - vy * h) - r));
        }
}

/* Distance to a rounded rectangle, negative inside. */
static inline float rrect_dist(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w / 2, cy = y + h / 2;
    float qx = fabsf(px - cx) - (w / 2 - r), qy = fabsf(py - cy) - (h / 2 - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return hypotf(ox, oy) + fminf(fmaxf(qx, qy), 0) - r;
}

static void rrect_fill(float x, float y, float w, float h, float r, unsigned char ink) {
    for (int j = (int)y - 1; j <= (int)(y + h) + 1; j++)
        for (int i = (int)x - 1; i <= (int)(x + w) + 1; i++)
            blend(i, j, ink, cover(rrect_dist(i + 0.5f, j + 0.5f, x, y, w, h, r)));
}

static void rrect_line(float x, float y, float w, float h, float r, float t, unsigned char ink) {
    for (int j = (int)(y - t); j <= (int)(y + h + t); j++)
        for (int i = (int)(x - t); i <= (int)(x + w + t); i++)
            blend(i, j, ink, cover(fabsf(rrect_dist(i + 0.5f, j + 0.5f, x, y, w, h, r)) - t / 2));
}

/* A filled convex or concave polygon by even-odd scanlines, with 4x vertical
 * supersampling for soft edges. */
static void poly_fill(const float *xy, int n, unsigned char ink) {
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < n; i++) { ymin = fminf(ymin, xy[2 * i + 1]); ymax = fmaxf(ymax, xy[2 * i + 1]); }
    static float acc[1100];
    for (int y = (int)ymin; y <= (int)ymax; y++) {
        if (y < 0 || y >= H) continue;
        memset(acc, 0, sizeof acc);
        for (int s = 0; s < 4; s++) {
            float sy = y + (s + 0.5f) / 4, xs[32];
            int k = 0;
            for (int i = 0, j = n - 1; i < n && k < 32; j = i++) {
                float yi = xy[2 * i + 1], yj = xy[2 * j + 1];
                if ((yi > sy) != (yj > sy)) xs[k++] = xy[2 * i] + (sy - yi) / (yj - yi) * (xy[2 * j] - xy[2 * i]);
            }
            for (int a = 1; a < k; a++) for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) { float t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
            for (int a = 0; a + 1 < k; a += 2)
                for (int x = (int)xs[a]; x <= (int)xs[a + 1] && x < W; x++) {
                    if (x < 0) continue;
                    float l = fmaxf(xs[a], x), r = fminf(xs[a + 1], x + 1);
                    if (r > l) acc[x] += (r - l) / 4;
                }
        }
        for (int x = 0; x < W; x++) if (acc[x] > 0) blend(x, y, ink, acc[x]);
    }
}

/* Ordered dithering of grey into the page, for shaded art that survives the
 * black-and-white A2 waveform. */
static const unsigned char bayer8[64] = {
    0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
    3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21};
static inline unsigned char dither(int x, int y, int grey) { return grey > bayer8[(y & 7) * 8 + (x & 7)] * 4 + 2 ? WHITE : BLACK; }

#endif

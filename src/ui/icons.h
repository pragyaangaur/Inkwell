/* icons.h: Inkwell's line-art app icons, drawn from gfx.h primitives at any
 * size. Each draws centred on (cx, cy) inside a square of side s. */
#ifndef ICONS_H
#define ICONS_H
#include "gfx.h"

/* An outline ellipse, from a scaled distance (close enough at icon sizes). */
static void ellipse_ring(float cx, float cy, float a, float b, float t, unsigned char ink) {
    float m = fminf(a, b);
    for (int y = (int)(cy - b - t); y <= (int)(cy + b + t); y++)
        for (int x = (int)(cx - a - t); x <= (int)(cx + a + t); x++) {
            float dx = (x + 0.5f - cx) / a, dy = (y + 0.5f - cy) / b;
            blend(x, y, ink, cover(fabsf((sqrtf(dx * dx + dy * dy) - 1) * m) - t / 2));
        }
}

static void icon_book(float cx, float cy, float s) {
    float t = s * 0.06f, w = s * 0.62f, h = s * 0.78f, x = cx - w / 2, y = cy - h / 2;
    rrect_line(x, y, w, h, s * 0.05f, t, BLACK);
    line(x + w * 0.2f, y + t, x + w * 0.2f, y + h - t, t * 0.8f, BLACK);
    float rb[] = {x + w * 0.55f, y, x + w * 0.75f, y, x + w * 0.75f, y + h * 0.42f, x + w * 0.65f, y + h * 0.34f, x + w * 0.55f, y + h * 0.42f};
    poly_fill(rb, 5, BLACK);
    line(x + w * 0.34f, y + h * 0.62f, x + w * 0.82f, y + h * 0.62f, t * 0.6f, 0x60);
    line(x + w * 0.34f, y + h * 0.74f, x + w * 0.72f, y + h * 0.74f, t * 0.6f, 0x60);
}

static void icon_skull(float cx, float cy, float s) {
    float t = s * 0.06f, r = s * 0.3f;
    arc(cx, cy - s * 0.06f, r, t, 2.6f, 6.83f, BLACK);
    line(cx - r * 0.86f, cy + r * 0.38f, cx - r * 0.55f, cy + r * 0.62f, t, BLACK);
    line(cx + r * 0.86f, cy + r * 0.38f, cx + r * 0.55f, cy + r * 0.62f, t, BLACK);
    rrect_line(cx - r * 0.55f, cy + r * 0.62f, r * 1.1f, r * 0.62f, r * 0.12f, t, BLACK);
    for (int i = 1; i < 4; i++) line(cx - r * 0.55f + r * 0.275f * i, cy + r * 0.66f, cx - r * 0.55f + r * 0.275f * i, cy + r * 1.2f, t * 0.6f, BLACK);
    disc(cx - r * 0.42f, cy + r * 0.02f, r * 0.24f, BLACK);
    disc(cx + r * 0.42f, cy + r * 0.02f, r * 0.24f, BLACK);
    float nose[] = {cx, cy + r * 0.26f, cx - r * 0.12f, cy + r * 0.5f, cx + r * 0.12f, cy + r * 0.5f};
    poly_fill(nose, 3, BLACK);
}

static void icon_globe(float cx, float cy, float s) {
    float t = s * 0.055f, r = s * 0.36f;
    ring(cx, cy, r, t, BLACK);
    ellipse_ring(cx, cy, r * 0.45f, r, t * 0.8f, BLACK);
    line(cx, cy - r, cx, cy + r, t * 0.8f, BLACK);
    line(cx - r, cy, cx + r, cy, t * 0.8f, BLACK);
    line(cx - r * 0.87f, cy - r * 0.5f, cx + r * 0.87f, cy - r * 0.5f, t * 0.6f, 0x50);
    line(cx - r * 0.87f, cy + r * 0.5f, cx + r * 0.87f, cy + r * 0.5f, t * 0.6f, 0x50);
}

static void icon_page(float cx, float cy, float s) {
    float t = s * 0.06f, w = s * 0.56f, h = s * 0.74f, x = cx - w / 2, y = cy - h / 2, f = w * 0.3f;
    line(x, y, x + w - f, y, t, BLACK);
    line(x + w - f, y, x + w, y + f, t, BLACK);
    line(x + w, y + f, x + w, y + h, t, BLACK);
    line(x + w, y + h, x, y + h, t, BLACK);
    line(x, y + h, x, y, t, BLACK);
    line(x + w - f, y, x + w - f, y + f, t * 0.7f, BLACK);
    line(x + w - f, y + f, x + w, y + f, t * 0.7f, BLACK);
    for (int i = 0; i < 3; i++) line(x + w * 0.2f, y + h * (0.45f + 0.16f * i), x + w * (i == 2 ? 0.6f : 0.8f), y + h * (0.45f + 0.16f * i), t * 0.6f, 0x50);
}

/* A paper with a bold X, for arXiv. */
static void icon_paper(float cx, float cy, float s) {
    float t = s * 0.06f, w = s * 0.56f, h = s * 0.74f, x = cx - w / 2, y = cy - h / 2;
    rrect_line(x, y, w, h, s * 0.03f, t, BLACK);
    line(x + w * 0.2f, y + h * 0.18f, x + w * 0.8f, y + h * 0.18f, t * 0.6f, 0x50);
    line(x + w * 0.2f, y + h * 0.3f, x + w * 0.65f, y + h * 0.3f, t * 0.6f, 0x50);
    line(x + w * 0.24f, y + h * 0.46f, x + w * 0.76f, y + h * 0.86f, t * 1.9f, BLACK);
    line(x + w * 0.76f, y + h * 0.46f, x + w * 0.24f, y + h * 0.86f, t * 1.1f, BLACK);
}

static void icon_pencil(float cx, float cy, float s) {
    float t = s * 0.055f, a = s * 0.32f, w = s * 0.09f;
    /* body along the diagonal from top right to bottom left */
    float ux = -0.7071f, uy = 0.7071f, nx = 0.7071f, ny = 0.7071f;
    float x0 = cx + a * 0.95f * -ux, y0 = cy + a * 0.95f * -uy, x1 = cx + a * 0.45f * ux, y1 = cy + a * 0.45f * uy;
    float body[] = {x0 + nx * w, y0 + ny * w, x1 + nx * w, y1 + ny * w, x1 - nx * w, y1 - ny * w, x0 - nx * w, y0 - ny * w};
    poly_fill(body, 4, BLACK);
    float tip[] = {x1 + nx * w, y1 + ny * w, cx + a * ux, cy + a * uy, x1 - nx * w, y1 - ny * w};
    float lead[] = {cx + a * 0.82f * ux + nx * w * 0.33f, cy + a * 0.82f * uy + ny * w * 0.33f, cx + a * ux, cy + a * uy, cx + a * 0.82f * ux - nx * w * 0.33f, cy + a * 0.82f * uy - ny * w * 0.33f};
    line(tip[0], tip[1], tip[2], tip[3], t * 0.8f, BLACK);
    line(tip[4], tip[5], tip[2], tip[3], t * 0.8f, BLACK);
    poly_fill(lead, 3, BLACK);
    line(x0 + nx * w, y0 + ny * w, x0 - nx * w, y0 - ny * w, t, 0x80);
    /* a stroke it has just drawn */
    arc(cx - a * 0.2f, cy + a * 1.15f, a * 0.55f, t * 0.7f, 3.6f, 5.3f, 0x60);
}

static void icon_wifi(float cx, float cy, float s) {
    float t = s * 0.07f, base = cy + s * 0.24f;
    for (int i = 1; i <= 3; i++) arc(cx, base, s * 0.14f * i, t, 3.93f, 5.50f, BLACK);
    disc(cx, base - t * 0.2f, t * 0.9f, BLACK);
}

static void icon_cube(float cx, float cy, float s) {
    float r = s * 0.34f, h = r * 0.5f;
    float top[] = {cx, cy - r, cx + r * 0.87f, cy - r + h, cx, cy - r + 2 * h, cx - r * 0.87f, cy - r + h};
    float left[] = {cx - r * 0.87f, cy - r + h, cx, cy - r + 2 * h, cx, cy + r, cx - r * 0.87f, cy + r - h};
    float right[] = {cx + r * 0.87f, cy - r + h, cx, cy - r + 2 * h, cx, cy + r, cx + r * 0.87f, cy + r - h};
    poly_fill(top, 4, 0xB0);
    poly_fill(left, 4, 0x50);
    poly_fill(right, 4, 0x20);
    float t = s * 0.03f;
    line(top[0], top[1], top[2], top[3], t, BLACK);
    line(top[2], top[3], top[4], top[5], t, BLACK);
    line(top[4], top[5], top[6], top[7], t, BLACK);
    line(top[6], top[7], top[0], top[1], t, BLACK);
}

static void icon_crown(float cx, float cy, float s) {
    float w = s * 0.62f, h = s * 0.42f, x = cx - w / 2, y = cy - h / 2 + s * 0.05f, t = s * 0.06f;
    float pts[] = {x, y + h, x, y, x + w * 0.27f, y + h * 0.5f, cx, y - h * 0.15f, x + w * 0.73f, y + h * 0.5f, x + w, y, x + w, y + h};
    for (int i = 0; i < 6; i++) line(pts[2 * i], pts[2 * i + 1], pts[2 * i + 2], pts[2 * i + 3], t, BLACK);
    line(x + w, y + h, x, y + h, t, BLACK);
    disc(x, y, t * 1.1f, BLACK);
    disc(cx, y - h * 0.15f, t * 1.1f, BLACK);
    disc(x + w, y, t * 1.1f, BLACK);
    line(x, y + h + t * 2, x + w, y + h + t * 2, t, BLACK);
}

static void icon_gear(float cx, float cy, float s) {
    float t = s * 0.06f, r = s * 0.22f;
    ring(cx, cy, r, t * 1.6f, BLACK);
    ring(cx, cy, r * 0.42f, t, BLACK);
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f;
        line(cx + cosf(a) * r * 1.1f, cy + sinf(a) * r * 1.1f, cx + cosf(a) * r * 1.55f, cy + sinf(a) * r * 1.55f, t * 2.2f, BLACK);
    }
}

static void icon_drop(float cx, float cy, float s) {
    float r = s * 0.24f;
    float tipy = cy - s * 0.4f;
    float body[] = {cx, tipy, cx + r * 0.95f, cy + s * 0.02f, cx - r * 0.95f, cy + s * 0.02f};
    disc(cx, cy + s * 0.12f, r, BLACK);
    poly_fill(body, 3, BLACK);
    disc(cx - r * 0.35f, cy + s * 0.1f, r * 0.22f, 0xE0);
}

/* A quill feather: a curved vane either side of a shaft, nib at bottom left. */
static void icon_quill(float cx, float cy, float s) {
    float x0 = cx - s * 0.3f, y0 = cy + s * 0.34f, x1 = cx + s * 0.28f, y1 = cy - s * 0.36f;
    float vx = x1 - x0, vy = y1 - y0, len = hypotf(vx, vy), ux = vx / len, uy = vy / len, nx = -uy, ny = ux;
    float pts[2 * 24];
    int n = 0;
    for (int i = 0; i <= 11; i++) { /* the left side of the vane, base to tip */
        float t = 0.18f + 0.82f * i / 11.0f, w = sinf(3.14159f * (t - 0.18f) / 0.82f) * s * 0.17f * (1.15f - t * 0.4f);
        pts[n++] = x0 + vx * t + nx * w; pts[n++] = y0 + vy * t + ny * w;
    }
    for (int i = 11; i >= 0; i--) { /* and back down the narrower right side */
        float t = 0.18f + 0.82f * i / 11.0f, w = sinf(3.14159f * (t - 0.18f) / 0.82f) * s * 0.09f;
        pts[n++] = x0 + vx * t - nx * w; pts[n++] = y0 + vy * t - ny * w;
    }
    poly_fill(pts, 24, BLACK);
    line(x0, y0, x1 - ux * s * 0.05f, y1 - uy * s * 0.05f, s * 0.035f, WHITE);
    line(x0, y0, x0 + vx * 0.22f, y0 + vy * 0.22f, s * 0.05f, BLACK);
    /* a few barbs cut into the vane */
    for (int i = 0; i < 3; i++) {
        float t = 0.42f + i * 0.15f;
        line(x0 + vx * t, y0 + vy * t, x0 + vx * (t - 0.06f) + nx * s * 0.2f, y0 + vy * (t - 0.06f) + ny * s * 0.2f, s * 0.022f, WHITE);
    }
}
static void icon_quill_small(float cx, float cy) { icon_quill(cx, cy, 52); }

static void icon_sun(float cx, float cy, float s, unsigned char ink) {
    float r = s * 0.2f, t = s * 0.09f;
    ring(cx, cy, r, t, ink);
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f;
        line(cx + cosf(a) * r * 1.6f, cy + sinf(a) * r * 1.6f, cx + cosf(a) * r * 2.2f, cy + sinf(a) * r * 2.2f, t, ink);
    }
}

/* Draws the named icon. Unknown names get the ink drop. */
/* A font glyph centred on (cx, cy), blended in like the line art. */
static void glyph_center(font *f, int px, int c, float cx, float cy, unsigned char ink) {
    const glyph *g = glyph_get(f, px, c);
    int x0 = (int)(cx - g->w / 2.0f), y0 = (int)(cy - g->h / 2.0f);
    for (int j = 0; j < g->h; j++)
        for (int i = 0; i < g->w; i++)
            if (g->bm[j * g->w + i]) blend(x0 + i, y0 + j, ink, g->bm[j * g->w + i] / 255.0f);
}

/* A terminal window with a prompt and a cursor. */
static void icon_terminal(float cx, float cy, float s) {
    float t = s * 0.06f, w = s * 0.72f, h = s * 0.56f, x = cx - w / 2, y = cy - h / 2;
    rrect_line(x, y, w, h, s * 0.07f, t, BLACK);
    line(x + w * 0.2f, y + h * 0.32f, x + w * 0.36f, y + h * 0.5f, t, BLACK);
    line(x + w * 0.36f, y + h * 0.5f, x + w * 0.2f, y + h * 0.68f, t, BLACK);
    line(x + w * 0.46f, y + h * 0.7f, x + w * 0.74f, y + h * 0.7f, t, BLACK);
}

/* A key lying on its side: a round bow, the shaft and two bits. */
static void icon_key(float cx, float cy, float s) {
    float t = s * 0.06f, r = s * 0.15f, bx = cx - s * 0.2f, x1 = cx + s * 0.34f;
    ring(bx, cy, r, t, BLACK);
    disc(bx, cy, r * 0.32f, BLACK);
    line(bx + r, cy, x1, cy, t, BLACK);
    line(x1 - t / 2, cy, x1 - t / 2, cy + s * 0.14f, t, BLACK);
    line(x1 - s * 0.13f, cy, x1 - s * 0.13f, cy + s * 0.1f, t, BLACK);
}

/* The black knight from the chess font. */
static void icon_knight(float cx, float cy, float s) { glyph_center(&f_chess, (int)(s * 1.1f), 0x265E, cx, cy, BLACK); }

/* A desk calendar with two rings and a marked day, for the Dashboard. */
static void icon_calendar(float cx, float cy, float s) {
    float t = s * 0.06f, w = s * 0.66f, h = s * 0.6f, x = cx - w / 2, y = cy - h / 2 + s * 0.04f;
    rrect_line(x, y, w, h, s * 0.06f, t, BLACK);
    rrect_fill(x, y, w, h * 0.26f, s * 0.06f, BLACK);
    for (int i = 0; i < 2; i++) line(x + w * (0.3f + 0.4f * i), y - s * 0.07f, x + w * (0.3f + 0.4f * i), y + s * 0.05f, t, BLACK);
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++) disc(x + w * (0.25f + 0.25f * c), y + h * (0.48f + 0.26f * r), s * 0.035f, 0x50);
    ring(x + w * 0.75f, y + h * 0.74f, s * 0.07f, t * 0.7f, BLACK);
}

/* ---- weather ------------------------------------------------------------------ */

/* A cloud's outline: three bumps on a flat base. */
static void cloud_shape(float cx, float cy, float s, float grow, unsigned char ink) {
    disc(cx - s * 0.2f, cy + s * 0.04f, s * 0.15f + grow, ink);
    disc(cx + s * 0.02f, cy - s * 0.06f, s * 0.21f + grow, ink);
    disc(cx + s * 0.22f, cy + s * 0.06f, s * 0.13f + grow, ink);
    rrect_fill(cx - s * 0.35f - grow, cy + s * 0.02f - grow, s * 0.7f + 2 * grow, s * 0.17f + 2 * grow, s * 0.08f + grow, ink);
}
static void icon_cloud(float cx, float cy, float s, unsigned char fill_ink) {
    cloud_shape(cx, cy, s, s * 0.045f, BLACK);
    cloud_shape(cx, cy, s, 0, fill_ink);
}

/* sun, partly, cloud, rain, snow, fog or storm, as dashboard-fetch names them. */
static void weather_icon(const char *name, float cx, float cy, float s) {
    float t = s * 0.05f;
    if (!strcmp(name, "sun")) { icon_sun(cx, cy, s, BLACK); return; }
    if (!strcmp(name, "partly")) {
        icon_sun(cx - s * 0.16f, cy - s * 0.14f, s * 0.75f, BLACK);
        icon_cloud(cx + s * 0.06f, cy + s * 0.08f, s * 0.85f, WHITE);
        return;
    }
    if (!strcmp(name, "fog")) {
        for (int i = 0; i < 4; i++) line(cx - s * (0.34f - 0.06f * (i & 1)), cy - s * 0.2f + i * s * 0.13f, cx + s * (0.34f - 0.06f * !(i & 1)), cy - s * 0.2f + i * s * 0.13f, t, BLACK);
        return;
    }
    int wet = !strcmp(name, "rain"), snow = !strcmp(name, "snow"), storm = !strcmp(name, "storm");
    float up = wet || snow || storm ? s * 0.12f : 0;
    icon_cloud(cx, cy - up, s, wet || storm ? 0xC0 : WHITE);
    float by = cy - up + s * 0.27f;
    if (wet) for (int i = 0; i < 3; i++) line(cx - s * 0.18f + i * s * 0.17f, by, cx - s * 0.23f + i * s * 0.17f, by + s * 0.16f, t, BLACK);
    if (snow) for (int i = 0; i < 3; i++) { disc(cx - s * 0.18f + i * s * 0.18f, by + s * 0.05f + (i & 1) * s * 0.08f, s * 0.035f, BLACK); }
    if (storm) {
        float bolt[] = {cx + s * 0.04f, by - s * 0.02f, cx - s * 0.08f, by + s * 0.12f, cx, by + s * 0.12f, cx - s * 0.06f, by + s * 0.26f,
                        cx + s * 0.1f, by + s * 0.08f, cx + s * 0.02f, by + s * 0.08f};
        poly_fill(bolt, 6, BLACK);
    }
}

static void icon(const char *name, float cx, float cy, float s) {
    if (!strcmp(name, "book")) icon_book(cx, cy, s);
    else if (!strcmp(name, "skull")) icon_skull(cx, cy, s);
    else if (!strcmp(name, "globe")) icon_globe(cx, cy, s);
    else if (!strcmp(name, "page")) icon_page(cx, cy, s);
    else if (!strcmp(name, "pencil")) icon_pencil(cx, cy, s);
    else if (!strcmp(name, "wifi")) icon_wifi(cx, cy, s);
    else if (!strcmp(name, "cube")) icon_cube(cx, cy, s);
    else if (!strcmp(name, "crown")) icon_crown(cx, cy, s);
    else if (!strcmp(name, "gear")) icon_gear(cx, cy, s);
    else if (!strcmp(name, "quill")) icon_quill(cx, cy, s);
    else if (!strcmp(name, "terminal")) icon_terminal(cx, cy, s);
    else if (!strcmp(name, "knight")) icon_knight(cx, cy, s);
    else if (!strcmp(name, "paper")) icon_paper(cx, cy, s);
    else if (!strcmp(name, "key")) icon_key(cx, cy, s);
    else if (!strcmp(name, "calendar")) icon_calendar(cx, cy, s);
    else icon_drop(cx, cy, s);
}

/* Battery outline with its fill level, for status bars. */
static void battery_glyph(float x, float y, float h, int pct, int charging) {
    float w = h * 2;
    rrect_line(x, y, w, h, h * 0.18f, h * 0.1f, BLACK);
    fill((int)(x + w + h * 0.05f), (int)(y + h * 0.3f), (int)(h * 0.15f), (int)(h * 0.4f), BLACK);
    float in = h * 0.22f;
    if (pct > 0) rrect_fill(x + in, y + in, (w - 2 * in) * (pct > 100 ? 100 : pct) / 100, h - 2 * in, h * 0.06f, BLACK);
    if (charging) {
        float bolt[] = {x + w * 0.55f, y + h * 0.05f, x + w * 0.35f, y + h * 0.55f, x + w * 0.5f, y + h * 0.55f,
                        x + w * 0.42f, y + h * 0.95f, x + w * 0.66f, y + h * 0.42f, x + w * 0.5f, y + h * 0.42f};
        for (int i = 0; i < 6; i++) bolt[2 * i] += 0;
        poly_fill(bolt, 6, WHITE);
        float in2[12];
        memcpy(in2, bolt, sizeof in2);
        for (int i = 0; i < 6; i++) line(in2[2 * i], in2[2 * i + 1], in2[(2 * i + 2) % 12], in2[(2 * i + 3) % 12], h * 0.05f, BLACK);
    }
}

#endif

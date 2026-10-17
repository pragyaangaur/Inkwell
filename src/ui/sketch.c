/* sketch: a drawing pad for a fingertip. Strokes go to the panel on the fast
 * black-and-white A2 waveform, batched every few dozen milliseconds so the
 * panel isn't swamped. The canvas is kept between visits in
 * /mnt/us/sketches/canvas.bmp, and Save writes a dated copy beside it. */
#include "ui.h"
#include <sys/stat.h>
#include <time.h>

#define DIR_SKETCH "/mnt/us/sketches"
#define BAR_H 150
#define FLUSH_MS 45

enum { T_FINE, T_PEN, T_MARKER, T_ERASER, NTOOLS };
static const float tool_size[NTOOLS] = {3, 7, 16, 48};
static int tool = T_PEN;
static rect tool_r[NTOOLS], clear_btn, save_btn;

/* ---- files -------------------------------------------------------------------- */

/* 8-bit greyscale BMP of the canvas area (everything under the toolbar). */
static int save_bmp(const char *path) {
    int w = W, h = H - BAR_H, stride = (w + 3) & ~3;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned hdr = 14 + 40 + 256 * 4, size = hdr + stride * h;
    unsigned char fh[14] = {'B', 'M'};
    memcpy(fh + 2, &size, 4);
    memcpy(fh + 10, &hdr, 4);
    int ih[10] = {40, w, h, 1 | (8 << 16), 0, stride * h, 11811, 11811, 256, 256};
    fwrite(fh, 1, 14, f);
    fwrite(ih, 4, 10, f);
    for (int i = 0; i < 256; i++) { unsigned char c[4] = {i, i, i, 0}; fwrite(c, 1, 4, f); }
    unsigned char pad[4] = {0};
    for (int y = H - 1; y >= BAR_H; y--) {
        fwrite(fb + y * PITCH, 1, w, f);
        fwrite(pad, 1, stride - w, f);
    }
    fclose(f);
    sync();
    return 0;
}

static int load_bmp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char fh[14];
    int ih[10];
    if (fread(fh, 1, 14, f) != 14 || fread(ih, 4, 10, f) != 10 || ih[1] != W || ih[2] != H - BAR_H) { fclose(f); return -1; }
    unsigned off;
    memcpy(&off, fh + 10, 4);
    fseek(f, off, SEEK_SET);
    int stride = (W + 3) & ~3;
    unsigned char *row = malloc(stride);
    for (int y = H - 1; y >= BAR_H; y--) {
        if (fread(row, 1, stride, f) != (size_t)stride) break;
        memcpy(fb + y * PITCH, row, W);
    }
    free(row);
    fclose(f);
    return 0;
}

/* ---- toolbar ------------------------------------------------------------------ */

static void draw_bar(void) {
    fill(0, 0, W, BAR_H, WHITE);
    float cx = back_btn.x + 52, cy = BAR_H / 2.0f;
    ring(cx, cy, 34, 3, BLACK);
    line(cx + 8, cy - 15, cx - 8, cy, 5, BLACK);
    line(cx - 8, cy, cx + 8, cy + 15, 5, BLACK);
    int x = 190;
    for (int t = 0; t < NTOOLS; t++) {
        tool_r[t] = (rect){x, 25, 100, 100};
        float tx = x + 50, ty = 75;
        if (t == tool) rrect_fill(x, 25, 100, 100, 22, 0xD0);
        if (t == T_ERASER) {
            rrect_line(tx - 26, ty - 16, 52, 32, 8, 4, BLACK);
            line(tx - 6, ty - 16, tx - 6, ty + 16, 3, BLACK);
        } else {
            disc(tx, ty, tool_size[t] / 2 + 1, BLACK);
        }
        x += 116;
    }
    clear_btn = (rect){W - MARGIN - 380, 35, 170, 80};
    save_btn = (rect){W - MARGIN - 190, 35, 190, 80};
    pill(clear_btn, "Clear", 0);
    pill(save_btn, "Save", 1);
    fill(0, BAR_H - 2, W, 2, BLACK);
}

/* ---- drawing ------------------------------------------------------------------ */

static int dx0 = 1 << 30, dy0 = 1 << 30, dx1 = -1, dy1 = -1;
static void dirty(int x0, int y0, int x1, int y1) {
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}
static void flush(void) {
    if (dx1 < 0) return;
    if (dy0 < BAR_H) dy0 = BAR_H;
    refresh(dx0, dy0, dx1 - dx0 + 1, dy1 - dy0 + 1, WF_A2, 0, 0);
    dx0 = dy0 = 1 << 30;
    dx1 = dy1 = -1;
}

/* A stroke segment. Ink is drawn hard-edged (no grey), since A2 shows only
 * black and white anyway and soft edges would come out ragged. */
static void segment(int x0, int y0, int x1, int y1) {
    float r = tool_size[tool] / 2;
    unsigned char ink = tool == T_ERASER ? WHITE : BLACK;
    int xa = (x0 < x1 ? x0 : x1) - r - 1, xb = (x0 > x1 ? x0 : x1) + r + 1;
    int ya = (y0 < y1 ? y0 : y1) - r - 1, yb = (y0 > y1 ? y0 : y1) + r + 1;
    if (ya < BAR_H) ya = BAR_H;
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int y = ya; y <= yb && y < H; y++)
        for (int x = xa < 0 ? 0 : xa; x <= xb && x < W; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float h = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            h = h < 0 ? 0 : h > 1 ? 1 : h;
            float ex = px - vx * h, ey = py - vy * h;
            if (ex * ex + ey * ey <= r * r) fb[y * PITCH + x] = ink;
        }
    dirty(xa, ya, xb, yb);
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    want_moves = 1;
    mkdir(DIR_SKETCH, 0755);
    fill(0, 0, W, H, WHITE);
    load_bmp(DIR_SKETCH "/canvas.bmp");
    draw_bar();
    flip_flash();
    int lx = -1, ly = -1, drawing = 0;
    for (;;) {
        event e = input_wait(dx1 >= 0 ? FLUSH_MS : -1);
        if (e.type == EV_NONE) { flush(); continue; }
        if (e.type == EV_POWER) break;
        if (e.type == EV_DOWN) {
            if (e.y >= BAR_H) { drawing = 1; lx = e.x; ly = e.y; segment(lx, ly, lx, ly); continue; }
            drawing = 0;
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(clear_btn, e.x, e.y)) press_on(clear_btn);
            else if (hit(save_btn, e.x, e.y)) press_on(save_btn);
            continue;
        }
        if (e.type == EV_MOVE) {
            if (drawing && e.y >= BAR_H) { segment(lx, ly, e.x, e.y); lx = e.x; ly = e.y; }
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (drawing) { drawing = 0; flush(); continue; }
        if (hit(back_btn, e.x, e.y)) { press_forget(); break; }
        if (hit(clear_btn, e.x, e.y)) {
            press_forget();
            fill(0, BAR_H, W, H - BAR_H, WHITE);
            draw_bar();
            flip_flash();
            continue;
        }
        if (hit(save_btn, e.x, e.y)) {
            press_off();
            char path[128];
            time_t now = time(0);
            strftime(path, sizeof path, DIR_SKETCH "/sketch-%Y%m%d-%H%M%S.bmp", localtime(&now));
            snapshot under = snap(60, H / 2 - 110, W - 120, 230);
            toast(save_bmp(path) == 0 ? "Saved to the sketches folder" : "Couldn't save");
            usleep(900000);
            unsnap(&under);
            refresh(60, H / 2 - 110, W - 120, 230, WF_GL16, 0, 0);
            continue;
        }
        int picked = 0;
        for (int t = 0; t < NTOOLS; t++)
            if (hit(tool_r[t], e.x, e.y)) { tool = t; draw_bar(); refresh(0, 0, W, BAR_H, WF_GL16, 0, 0); picked = 1; }
        if (!picked) press_off();
    }
    flush();
    save_bmp(DIR_SKETCH "/canvas.bmp");
    return 0;
}

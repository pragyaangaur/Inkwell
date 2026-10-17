#include "rd.h"
#include "kio.h"

/* ---- the control pad --------------------------------------------------- */

enum { B_FWD, B_BACK, B_LEFT, B_RIGHT, B_JUMP, B_DIG, B_PLACE, B_BLOCK,
       B_LOOK_UP, B_LOOK_DOWN, B_TURN_L, B_TURN_R, B_MODE, B_RESET, B_QUIT, NB };
typedef struct { int x, y, w, h; const char *label; } button;
static button pad[NB];
static int VIEW; /* the game view is VIEW x VIEW at the top */

static void layout(void) {
    VIEW = W < H ? W : H;
    if (VIEW > H - 340) VIEW = H - 340;
    int top = VIEW + 8, h = H - top - 8;
    int rh = (h - 16) / 3, cw = (W - 40) / 7;
    /* left: movement cross; middle: actions; right: look cross */
    #define AT(b, c, r, cs, l) pad[b] = (button){8 + (c) * (cw + 4), top + (r) * (rh + 8), (cs) * cw + ((cs) - 1) * 4, rh, l}
    AT(B_FWD, 1, 0, 1, "W");
    AT(B_LEFT, 0, 1, 1, "A");
    AT(B_BACK, 1, 1, 1, "S");
    AT(B_RIGHT, 2, 1, 1, "D");
    AT(B_JUMP, 0, 2, 3, "JUMP");
    AT(B_DIG, 3, 0, 1, "DIG");
    AT(B_PLACE, 3, 1, 1, "PUT");
    AT(B_BLOCK, 3, 2, 1, "BLK");
    AT(B_LOOK_UP, 5, 0, 1, "UP");
    AT(B_TURN_L, 4, 1, 1, "L");
    AT(B_LOOK_DOWN, 5, 1, 1, "DN");
    AT(B_TURN_R, 6, 1, 1, "R");
    AT(B_MODE, 4, 0, 1, "GRAY");
    AT(B_RESET, 6, 0, 1, "NEW");
    AT(B_QUIT, 4, 2, 3, "QUIT");
}

static const char *block_name[] = {"", "STONE", "GRASS", "DIRT", "COBBLE", "PLANKS", "LOG", "LEAVES"};

static void draw_pad(int sel, int gray, const int *held) {
    fill(0, VIEW, W, H - VIEW, 0xFF);
    fill(0, VIEW, W, 4, 0x00);
    for (int i = 0; i < NB; i++) {
        button *b = &pad[i];
        const char *l = b->label;
        if (i == B_MODE) l = gray ? "GRAY" : "FAST";
        if (i == B_BLOCK) l = block_name[sel];
        int on = held[i];
        fill(b->x, b->y, b->w, b->h, on ? 0x00 : 0xFF);
        frame_rect(b->x, b->y, b->w, b->h, 4, 0x00);
        int s = 5;
        while (s > 2 && text_w(l, s) > b->w - 16) s--;
        text(b->x + (b->w - text_w(l, s)) / 2, b->y + (b->h - 7 * s) / 2, s, l, on ? 0xFF : 0x00);
    }
}

static int hit(int x, int y) {
    for (int i = 0; i < NB; i++)
        if (x >= pad[i].x && x < pad[i].x + pad[i].w && y >= pad[i].y && y < pad[i].y + pad[i].h) return i;
    return -1;
}

/* ---- dithering -------------------------------------------------------- */

static const u8 bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

#define RW 268 /* render size; the view is drawn at 4x */
static u32 frame[RW * RW];

static void blit(int gray) {
    int s = VIEW / RW, off = (VIEW - RW * s) / 2;
    for (int y = 0; y < RW; y++)
        for (int x = 0; x < RW; x++) {
            u32 c = frame[y * RW + x];
            int l = (int)(((c >> 16 & 255) * 77 + (c >> 8 & 255) * 150 + (c & 255) * 29) >> 8);
            for (int j = 0; j < s; j++) {
                int yy = off + y * s + j;
                for (int i = 0; i < s; i++) {
                    int xx = off + x * s + i;
                    u8 g;
                    if (gray) g = (u8)((l & 0xF0) | (l >> 4)); /* 16 panel levels */
                    else g = l * 16 / 256 > bayer[yy & 3][xx & 3] ? 0xFF : 0x00;
                    px(xx, yy, g);
                }
            }
        }
}

/* ---- main ------------------------------------------------------------- */

static int app_main(void) {
    if (fb_init() < 0) return 1;
    layout();
    open_input();
    if (touchfd < 0) say("rd-kindle: no touchscreen found\n");

    rd_init((u32)(now() * 1000) | 1, RD_HILLS);
    rd_set_view_distance(24);
    int me = rd_player_add();

    int gray = 0, held[NB] = {0}, prev_held[NB] = {0}, frames = 0, pad_dirty = 1;
    fill(0, 0, W, H, 0xFF);
    refresh(0, 0, W, H, WF_GC16, 1, 1);

    double t = now(), acc = 0;
    for (;;) {
        if (poll_input()) break;

        /* turn touch slots into button holds and view drags */
        rd_input in;
        memset(&in, 0, sizeof in);
        memset(held, 0, sizeof held);
        int quit = 0;
        for (int i = 0; i < NSLOT; i++) {
            if (slot[i].id < 0) { slot[i].btn = -1; continue; }
            if (slot[i].fresh) {
                slot[i].fresh = 0;
                slot[i].btn = slot[i].y < VIEW ? NB : hit(slot[i].x, slot[i].y);
                slot[i].px = slot[i].x; slot[i].py = slot[i].y;
                int b = slot[i].btn;
                if (b == B_DIG) in.dig = 1;
                if (b == B_PLACE) in.place = 1;
                if (b == B_BLOCK) in.select = rd_selected(me) % (RD_NUM_BLOCKS - 1) + 1, pad_dirty = 1;
                if (b == B_MODE) gray = !gray, pad_dirty = 1;
                if (b == B_RESET) in.reset = 1;
                if (b == B_QUIT) quit = 1;
            }
            if (slot[i].btn == NB) { /* drag on the view to look */
                in.yaw += (slot[i].x - slot[i].px) * 0.006f;
                in.pitch -= (slot[i].y - slot[i].py) * 0.006f;
                slot[i].px = slot[i].x; slot[i].py = slot[i].y;
            } else if (slot[i].btn >= 0) held[slot[i].btn] = 1;
        }
        if (quit) break;
        in.forward = (float)(held[B_FWD] - held[B_BACK]);
        in.strafe = (float)(held[B_RIGHT] - held[B_LEFT]);
        in.jump = held[B_JUMP];

        double t2 = now(), dt = t2 - t;
        t = t2;
        if (dt > 0.5) dt = 0.5;
        float turn = (float)(dt * 1.6);
        in.yaw += (held[B_TURN_R] - held[B_TURN_L]) * turn;
        in.pitch += (held[B_LOOK_UP] - held[B_LOOK_DOWN]) * turn;
        rd_player_input(me, &in);
        for (acc += dt; acc >= 1.0 / 60; acc -= 1.0 / 60) rd_tick();

        for (int i = 0; i < NB; i++) if (held[i] != prev_held[i]) pad_dirty = 1, prev_held[i] = held[i];
        if (pad_dirty) {
            draw_pad(rd_selected(me), gray, held);
            refresh(0, VIEW, W, H - VIEW, WF_DU, 0, 0);
            pad_dirty = 0;
        }

        rd_render(me, frame, RW, RW, RW);
        blit(gray);
        frames++;
        if (frames % 120 == 0) refresh(0, 0, W, VIEW, WF_GC16, 1, 1); /* clear ghosting */
        else refresh(0, 0, W, VIEW, gray ? WF_GC16 : WF_A2, 0, 1);
    }

    fill(0, 0, W, H, 0xFF);
    refresh(0, 0, W, H, WF_GC16, 1, 1);
    return 0;
}


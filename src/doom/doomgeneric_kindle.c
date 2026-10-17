/* doomgeneric_kindle.c: Doom on Inkwell. The game draws 320x200 at the top
 * of the portrait screen, scaled to 1072x670 and dithered to black and white
 * so each frame can go out on the 190 ms A2 waveform (about five frames a
 * second on the panel while the game itself runs at 35). Below it sit
 * multitouch controls: a d-pad, Fire, Use, Weapon, and a row of small keys.
 * The power button quits back to the home screen. */
#include "../ui/ui.h"
/* After ui.h, so Doom's key codes replace linux/input.h's names that clash
 * (KEY_ENTER and KEY_TAB mean different numbers to Doom). */
#include "doomgeneric.h"
#include "doomkeys.h"
#include <sys/stat.h>
#include <time.h>

#define GAME_H 670
#define FRAME_MS 200

/* ---- controls ------------------------------------------------------------------ */

enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_FIRE, B_USE, B_WEAPON, B_MENU, B_MAP, B_ENTER, B_YES, B_RUN, B_SL, B_SR, NB };
static const unsigned char bkey[NB] = {KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW, KEY_FIRE, KEY_USE, '2',
                                       KEY_ESCAPE, KEY_TAB, KEY_ENTER, 'y', KEY_RSHIFT, KEY_STRAFE_L, KEY_STRAFE_R};
static const char *const small_label[] = {"menu", "map", "enter", "yes", "run", "«", "»"};

#define STRIP_Y 692
#define STRIP_H 96
#define PAD_X 268
#define PAD_Y 1110
#define PAD_R 235
#define FIRE_X 830
#define FIRE_Y 1010
#define FIRE_R 150
#define USE_X 700
#define USE_Y 1300
#define WPN_X 950
#define WPN_Y 1300
#define SMALL_R 92

static int weapon = 2, run_on;

static void draw_controls(void) {
    fill(0, GAME_H, W, H - GAME_H, WHITE);
    fill(0, GAME_H, W, 4, BLACK);
    int sw = (W - 2 * 20 - 6 * 12) / 7;
    for (int i = 0; i < 7; i++) {
        rect r = {20 + i * (sw + 12), STRIP_Y + 8, sw, STRIP_H - 16};
        int on = (i == 4 && run_on);
        rrect_fill(r.x, r.y, r.w, r.h, 18, on ? BLACK : 0xE0);
        text_center(&f_ui_bold, 30, r.x + r.w / 2, r.y + 20, small_label[i], on ? WHITE : BLACK);
    }
    /* d-pad */
    ring(PAD_X, PAD_Y, PAD_R, 4, BLACK);
    ring(PAD_X, PAD_Y, 70, 3, 0x80);
    for (int d = 0; d < 4; d++) {
        float a = d * 1.5708f - 1.5708f, cx = PAD_X + cosf(a) * 155, cy = PAD_Y + sinf(a) * 155;
        float tri[] = {cx + cosf(a) * 42, cy + sinf(a) * 42, cx + cosf(a + 2.2f) * 42, cy + sinf(a + 2.2f) * 42, cx + cosf(a - 2.2f) * 42, cy + sinf(a - 2.2f) * 42};
        poly_fill(tri, 3, BLACK);
    }
    /* fire */
    disc(FIRE_X, FIRE_Y, FIRE_R, BLACK);
    text_center(&f_heavy, 58, FIRE_X, FIRE_Y - 36, "FIRE", WHITE);
    ring(USE_X, USE_Y, SMALL_R, 5, BLACK);
    text_center(&f_ui_bold, 38, USE_X, USE_Y - 24, "use", BLACK);
    ring(WPN_X, WPN_Y, SMALL_R, 5, BLACK);
    char w[16];
    snprintf(w, sizeof w, "gun %d", weapon);
    text_center(&f_ui_bold, 34, WPN_X, WPN_Y - 22, w, BLACK);
    refresh(0, GAME_H, W, H - GAME_H, WF_GL16, 0, 0);
}

/* Which buttons a finger at (x, y) is holding. The d-pad gives diagonals. */
static unsigned buttons_at(int x, int y) {
    unsigned b = 0;
    if (y >= STRIP_Y && y < STRIP_Y + STRIP_H) {
        int sw = (W - 2 * 20 - 6 * 12) / 7, i = (x - 20) / (sw + 12);
        static const int map[7] = {B_MENU, B_MAP, B_ENTER, B_YES, B_RUN, B_SL, B_SR};
        if (i >= 0 && i < 7) b |= 1u << map[i];
        return b;
    }
    float dx = x - PAD_X, dy = y - PAD_Y, d = hypotf(dx, dy);
    if (d < PAD_R + 60 && d > 40) {
        if (dy < -d * 0.38f) b |= 1u << B_UP;
        if (dy > d * 0.38f) b |= 1u << B_DOWN;
        if (dx < -d * 0.38f) b |= 1u << B_LEFT;
        if (dx > d * 0.38f) b |= 1u << B_RIGHT;
        return b;
    }
    if (hypotf(x - FIRE_X, y - FIRE_Y) < FIRE_R + 40) return 1u << B_FIRE;
    if (hypotf(x - USE_X, y - USE_Y) < SMALL_R + 30) return 1u << B_USE;
    if (hypotf(x - WPN_X, y - WPN_Y) < SMALL_R + 30) return 1u << B_WEAPON;
    return 0;
}

/* ---- key queue -------------------------------------------------------------------- */

static unsigned short keyq[64];
static int kq_head, kq_tail;
static void key_push(int pressed, unsigned char k) {
    int next = (kq_tail + 1) % 64;
    if (next != kq_head) { keyq[kq_tail] = (pressed << 8) | k; kq_tail = next; }
}

/* ---- multitouch ------------------------------------------------------------------- */

#define SLOTS 10
static struct { int id, x, y; } slot[SLOTS];
static int cur_slot;
static unsigned held;

static void apply_buttons(void) {
    unsigned now = 0;
    for (int i = 0; i < SLOTS; i++)
        if (slot[i].id >= 0) now |= buttons_at(slot[i].x, slot[i].y);
    unsigned changed = now ^ held;
    for (int b = 0; b < NB; b++) {
        if (!(changed & (1u << b))) continue;
        int down = (now >> b) & 1;
        if (b == B_RUN) {
            if (down) { run_on = !run_on; key_push(run_on, KEY_RSHIFT); draw_controls(); }
            continue;
        }
        if (b == B_WEAPON) {
            if (down) {
                weapon = weapon % 7 + 1;
                key_push(1, '0' + weapon);
                key_push(0, '0' + weapon);
                draw_controls();
            }
            continue;
        }
        key_push(down, bkey[b]);
    }
    held = now;
}

static void read_touch(void) {
    struct input_event ev[64];
    long n;
    while (keyfd >= 0 && (n = read(keyfd, ev, sizeof ev)) > 0)
        for (int i = 0; i < n / (long)sizeof *ev; i++)
            if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1) exit(0);
    while (touchfd >= 0 && (n = read(touchfd, ev, sizeof ev)) > 0)
        for (int i = 0; i < n / (long)sizeof *ev; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_ABS) {
                if (e->code == ABS_MT_SLOT) cur_slot = e->value >= 0 && e->value < SLOTS ? e->value : 0;
                else if (e->code == ABS_MT_TRACKING_ID) slot[cur_slot].id = e->value;
                else if (e->code == ABS_MT_POSITION_X) slot[cur_slot].x = (e->value - tx0) * (W - 1) / (tx1 - tx0);
                else if (e->code == ABS_MT_POSITION_Y) slot[cur_slot].y = (e->value - ty0) * (H - 1) / (ty1 - ty0);
            } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                apply_buttons();
            }
        }
}

/* ---- doomgeneric hooks ------------------------------------------------------------- */

static int xmap[1088], ymap[GAME_H];
static unsigned char gamma_lut[256];
static uint32_t last_frame;

void DG_Init(void) {
    if (fb_open() || fonts_load()) exit(1);
    input_open();
    for (int i = 0; i < SLOTS; i++) slot[i].id = -1;
    for (int x = 0; x < W; x++) xmap[x] = x * DOOMGENERIC_RESX / W;
    for (int y = 0; y < GAME_H; y++) ymap[y] = y * DOOMGENERIC_RESY / GAME_H;
    /* Doom is dark, and dithering loses the shadows, so lift the midtones. */
    for (int i = 0; i < 256; i++) { float v = (powf(i / 255.0f, 0.8f) - 0.04f) * 1.15f; gamma_lut[i] = (unsigned char)(v < 0 ? 0 : v > 1 ? 255 : v * 255); }
    fill(0, 0, W, H, WHITE);
    draw_controls();
    flip_flash();
}

void DG_DrawFrame(void) {
    read_touch();
    uint32_t now = DG_GetTicksMs();
    if (now - last_frame < FRAME_MS) return;
    last_frame = now;
    for (int y = 0; y < GAME_H; y++) {
        const uint32_t *src = DG_ScreenBuffer + ymap[y] * DOOMGENERIC_RESX;
        unsigned char *dst = fb + y * PITCH;
        const unsigned char *b = bayer8 + (y & 7) * 8;
        for (int x = 0; x < W; x++) {
            uint32_t p = src[xmap[x]];
            int r = (p >> 16) & 255, g = (p >> 8) & 255, bl = p & 255, mx = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
            /* part brightness, part luminance: pure luminance turns Doom's red
             * menu text and blood almost black */
            int l = gamma_lut[(mx * 5 + ((r * 77 + g * 150 + bl * 29) >> 8) * 3) >> 3];
            dst[x] = l > b[x & 7] * 4 + 2 ? WHITE : BLACK;
        }
    }
    refresh(0, 0, W, GAME_H, WF_A2, 0, 0);
}

void DG_SleepMs(uint32_t ms) {
    read_touch();
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    read_touch();
    if (kq_head == kq_tail) return 0;
    unsigned short k = keyq[kq_head];
    kq_head = (kq_head + 1) % 64;
    *pressed = k >> 8;
    *key = k & 255;
    return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    /* Saves and the config live on the userstore. */
    mkdir("/mnt/us/doom", 0755);
    if (chdir("/mnt/us/doom") == 0) setenv("HOME", "/mnt/us/doom", 1);
    char *args[] = {argv[0], "-iwad", "/usr/local/share/doom/doom1.wad", 0};
    if (argc < 2) { argc = 3; argv = args; }
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
}

/* ui.h: Inkwell's widgets, built on eink.h and gfx.h. A page header with a
 * back chevron, pill buttons, instant press feedback, toasts, and the
 * on-screen keyboard with a text entry screen. Include it once. */
#ifndef UI_H
#define UI_H
#include "eink.h"
#include "gfx.h"
#include "icons.h"
#include "sleep.h"

#define MARGIN 56
#define HEADER_H 150

typedef struct { int x, y, w, h; } rect;

static int hit(rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* ---- press feedback ------------------------------------------------------- */

/* A control goes black the moment a finger lands on it (A2, 190 ms, never
 * waited for) and comes back when the finger lifts. */
static snapshot pressed;
static rect pressed_r;

static void press_on(rect r) {
    if (pressed.px) unsnap(&pressed);
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.x + r.w > W) r.w = W - r.x;
    pressed = snap(r.x, r.y, r.w, r.h);
    pressed_r = r;
    invert(r.x, r.y, r.w, r.h);
    refresh(r.x, r.y, r.w, r.h, WF_A2, 0, 0);
}
/* Puts the control back as it was, for when the action keeps the page. */
static void press_off(void) {
    if (!pressed.px) return;
    unsnap(&pressed);
    refresh(pressed_r.x, pressed_r.y, pressed_r.w, pressed_r.h, WF_GL16, 0, 0);
}
/* Drops the saved pixels, for when the action redraws the page anyway. */
static void press_forget(void) {
    if (pressed.px) { free(pressed.px); pressed.px = 0; }
}

/* ---- pieces ----------------------------------------------------------------- */

static void pill(rect r, const char *label, int filled) {
    char fit[96];
    int px = r.h > 90 ? 38 : 32;
    if (filled) rrect_fill(r.x, r.y, r.w, r.h, r.h / 2.0f, BLACK);
    else {
        rrect_fill(r.x, r.y, r.w, r.h, r.h / 2.0f, WHITE);
        rrect_line(r.x + 1.5f, r.y + 1.5f, r.w - 3, r.h - 3, r.h / 2.0f - 1.5f, 3, BLACK);
    }
    text_fit(&f_ui_bold, px, label, r.w - 40, fit, sizeof fit);
    text_center(&f_ui_bold, px, r.x + r.w / 2, r.y + (r.h - px) / 2 - px / 10, fit, filled ? WHITE : BLACK);
}

static const rect back_btn = {16, 28, 150, 110};

/* Clears the page and draws a title with a back chevron and a fine rule. */
static void header(const char *title) {
    fill(0, 0, W, H, WHITE);
    float cx = back_btn.x + 52, cy = back_btn.y + back_btn.h / 2.0f;
    ring(cx, cy, 34, 3, BLACK);
    line(cx + 8, cy - 15, cx - 8, cy, 5, BLACK);
    line(cx - 8, cy, cx + 8, cy + 15, 5, BLACK);
    text(&f_serif_bold, 76, back_btn.x + 110, 38, title, BLACK);
    fill(MARGIN, HEADER_H - 2, W - 2 * MARGIN, 2, BLACK);
}

/* A thin rule with a diamond in the middle. */
static void ornament(int y) {
    int cx = W / 2;
    fill(MARGIN, y, cx - MARGIN - 26, 2, BLACK);
    fill(cx + 26, y, W - MARGIN - cx - 26, 2, BLACK);
    float d[] = {cx, y - 11, cx + 11, y + 1, cx, y + 13, cx - 11, y + 1};
    poly_fill(d, 4, BLACK);
}

/* A short message in a rounded card in the middle of the page. */
static void toast(const char *msg) {
    int w = W - 2 * MARGIN - 60, h = 170, x = (W - w) / 2, y = H / 2 - h / 2;
    rrect_fill(x + 8, y + 10, w, h, 28, 0x80);
    rrect_fill(x, y, w, h, 28, WHITE);
    rrect_line(x + 2, y + 2, w - 4, h - 4, 26, 4, BLACK);
    char fit[200];
    text_fit(&f_ui_bold, 40, msg, w - 60, fit, sizeof fit);
    text_center(&f_ui_bold, 40, W / 2, y + 62, fit, BLACK);
    refresh(x, y, w + 12, h + 14, WF_GC16, 0, 0);
}

/* ---- keyboard ----------------------------------------------------------- */

enum { K_NONE = 0, K_BACK = -1, K_ENTER = -2, K_SHIFT = -3, K_SYMBOLS = -4, K_SPACE = -5 };

static const char *const kb_letters[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *const kb_symbols[4] = {"1234567890", "!@#$%^&*()", "-_=+[]{};:", "'\",.?/\\|"};
static int kb_shift, kb_sym;
static const char *kb_enter_label = "Done";

#define KB_KEY_H 112
#define KB_GAP 10
#define KB_TOP (H - 5 * (KB_KEY_H + KB_GAP) - 26)

typedef struct { rect r; int code; char label[12]; } key;
static key kb_keys[64];
static int kb_nkeys;

static void kb_add(int x, int y, int w, int code, const char *label) {
    key *k = &kb_keys[kb_nkeys++];
    k->r = (rect){x, y, w, KB_KEY_H};
    k->code = code;
    snprintf(k->label, sizeof k->label, "%s", label);
}

/* Lays out the keys for the current layer (letters or symbols, shift or not). */
static void kb_layout(void) {
    const char *const *rows = kb_sym ? kb_symbols : kb_letters;
    int kw = (W - 2 * 22 - 9 * KB_GAP) / 10;
    kb_nkeys = 0;
    for (int r = 0; r < 4; r++) {
        int n = strlen(rows[r]), y = KB_TOP + r * (KB_KEY_H + KB_GAP);
        int x = (W - (n * kw + (n - 1) * KB_GAP)) / 2;
        if (r == 3) x = 22 + kw * 3 / 2 + KB_GAP;
        for (int i = 0; i < n; i++) {
            char c = rows[r][i], lab[2] = {c, 0};
            if (kb_shift && c >= 'a' && c <= 'z') lab[0] = c - 32;
            kb_add(x + i * (kw + KB_GAP), y, kw, (unsigned char)lab[0], lab);
        }
    }
    int y3 = KB_TOP + 3 * (KB_KEY_H + KB_GAP), y4 = y3 + KB_KEY_H + KB_GAP;
    kb_add(22, y3, kw * 3 / 2, K_SHIFT, kb_shift ? "SHIFT" : "shift");
    kb_add(W - 22 - kw * 3 / 2, y3, kw * 3 / 2, K_BACK, "del");
    kb_add(22, y4, kw * 2, K_SYMBOLS, kb_sym ? "abc" : "?123");
    kb_add(22 + kw * 2 + KB_GAP, y4, W - 44 - kw * 5 - 2 * KB_GAP, K_SPACE, "");
    kb_add(W - 22 - kw * 3, y4, kw * 3, K_ENTER, kb_enter_label);
}

static void kb_draw(void) {
    kb_layout();
    fill(0, KB_TOP - 22, W, H - KB_TOP + 22, 0xE8);
    for (int i = 0; i < kb_nkeys; i++) {
        key *k = &kb_keys[i];
        int special = k->code < 0;
        rrect_fill(k->r.x, k->r.y + 4, k->r.w, k->r.h, 14, 0xA8);
        rrect_fill(k->r.x, k->r.y, k->r.w, k->r.h, 14, k->code == K_ENTER ? BLACK : special ? 0xC8 : WHITE);
        if (k->code == K_SPACE) { fill(k->r.x + k->r.w / 2 - 60, k->r.y + k->r.h - 34, 120, 4, 0x60); continue; }
        int px = strlen(k->label) > 1 ? 34 : 50;
        text_center(strlen(k->label) > 1 ? &f_ui_bold : &f_ui, px, k->r.x + k->r.w / 2, k->r.y + (KB_KEY_H - px) / 2 - 6, k->label,
                    k->code == K_ENTER ? WHITE : BLACK);
    }
}
static void kb_refresh(void) { refresh(0, KB_TOP - 22, W, H - KB_TOP + 22, WF_GL16, 0, 0); }

/* Feedback for a finger landing on the keyboard. */
static void kb_down(int x, int y) {
    for (int i = 0; i < kb_nkeys; i++)
        if (hit(kb_keys[i].r, x, y)) { press_on(kb_keys[i].r); return; }
}

/* The key for a finished tap, or K_NONE. Shift and the symbol layer are
 * handled here and redraw the keyboard. */
static int kb_tap(int x, int y) {
    for (int i = 0; i < kb_nkeys; i++) {
        key *k = &kb_keys[i];
        if (!hit(k->r, x, y)) continue;
        int code = k->code; /* kb_draw below relays the keys, so read it first */
        if (code == K_SHIFT || code == K_SYMBOLS) {
            press_forget();
            if (code == K_SHIFT) kb_shift = !kb_shift;
            else kb_sym = !kb_sym, kb_shift = 0;
            kb_draw();
            kb_refresh();
            return K_NONE;
        }
        if (code > 0 && kb_shift) {
            press_forget();
            kb_shift = 0;
            kb_draw();
            kb_refresh();
        } else {
            press_off();
        }
        return code == K_SPACE ? ' ' : code;
    }
    press_off();
    return K_NONE;
}

/* ---- text entry screen ---------------------------------------------------- */

#define FIELD_Y 360
#define FIELD_H 120

static void field_draw(const char *buf, int secret, int shown) {
    char vis[256], fit[260];
    int n = strlen(buf);
    if (secret && !shown) {
        int k = 0;
        for (int i = 0; i < n && k < (int)sizeof vis - 4; i++) { memcpy(vis + k, "•", 3); k += 3; }
        vis[k] = 0;
    } else {
        snprintf(vis, sizeof vis, "%s", buf);
    }
    /* Keep the end of long text visible, since that's where typing happens. */
    const char *p = vis;
    while (*p && text_width(&f_ui, 52, p) > W - 2 * MARGIN - 80) { p++; while ((*p & 0xC0) == 0x80) p++; }
    snprintf(fit, sizeof fit, "%s", p);
    rrect_fill(MARGIN, FIELD_Y, W - 2 * MARGIN, FIELD_H, 24, 0xF0);
    rrect_line(MARGIN + 1.5f, FIELD_Y + 1.5f, W - 2 * MARGIN - 3, FIELD_H - 3, 23, 3, BLACK);
    int end = text(&f_ui, 52, MARGIN + 30, FIELD_Y + 30, fit, BLACK);
    fill(end + 4, FIELD_Y + 26, 4, 66, BLACK);
}

/* A full-screen prompt. Returns 1 when the user taps the enter key, 0 on
 * back. With secret set the text is masked and a Show button reveals it.
 * Keys type on the fast A2 waveform, and the field is redrawn in full grey
 * once typing pauses. */
static int text_entry(const char *title, const char *prompt, char *buf, int n, int secret) {
    const rect show_btn = {W - MARGIN - 200, FIELD_Y + FIELD_H + 28, 200, 80};
    int shown = 0, dirty = 0;
    kb_shift = kb_sym = 0;
    header(title);
    text(&f_ui, 38, MARGIN, 220, prompt, 0x40);
    field_draw(buf, secret, shown);
    if (secret) pill(show_btn, "Show", 0);
    kb_draw();
    flip();
    for (;;) {
        event e = input_wait(dirty ? 600 : -1);
        if (e.type == EV_NONE) {
            if (dirty) { refresh(MARGIN, FIELD_Y, W - 2 * MARGIN, FIELD_H, WF_GL16, 0, 0); dirty = 0; }
            continue;
        }
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (secret && hit(show_btn, e.x, e.y)) press_on(show_btn);
            else kb_down(e.x, e.y);
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) { press_forget(); return 0; }
        if (secret && hit(show_btn, e.x, e.y)) {
            press_forget();
            shown = !shown;
            pill(show_btn, shown ? "Hide" : "Show", 0);
            field_draw(buf, secret, shown);
            refresh(MARGIN, FIELD_Y, W - 2 * MARGIN, FIELD_H + 28 + 80, WF_GL16, 0, 0);
            continue;
        }
        int c = kb_tap(e.x, e.y), len = strlen(buf);
        if (c == K_NONE) continue;
        if (c == K_ENTER) return 1;
        if (c == K_BACK) {
            while (len > 0 && (buf[len - 1] & 0xC0) == 0x80) len--;
            if (len > 0) buf[len - 1] = 0;
        } else if (c > 0 && len < n - 1) {
            buf[len] = (char)c;
            buf[len + 1] = 0;
        }
        field_draw(buf, secret, shown);
        refresh(MARGIN, FIELD_Y, W - 2 * MARGIN, FIELD_H, WF_A2, 0, 0);
        dirty = 1;
    }
}

#endif

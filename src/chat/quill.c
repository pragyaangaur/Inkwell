/* quill: Inkwell's AI chatbot. A small language model (SmolLM2) runs on the
 * Kindle itself through inkllm; this app is the conversation around it. Your
 * messages sit on the right, Quill's answers stream in on the left word by
 * word, and the header switches between the quick and the smart model. */
#include "../ui/ui.h"
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#define MODELS "/mnt/us/linux/models/"
#define CONF "/mnt/us/linux/quill.model"
#define MAXMSG 64

typedef struct { int me; char *text; } msg;
static msg msgs[MAXMSG];
static int nmsgs;

static int smart, ready, busy;
static int llm_in = -1, llm_out = -1;
static pid_t llm_pid;

/* ---- the model process ------------------------------------------------------------ */

static void llm_start(void) {
    int to[2], from[2];
    if (pipe(to) || pipe(from)) return;
    llm_pid = fork();
    if (llm_pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        close(to[1]);
        close(from[0]);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 2);
        const char *m = smart ? MODELS "smollm2-360m-q4_0.gguf" : MODELS "smollm2-135m-q4_0.gguf";
        const char *c = smart ? MODELS "quill-360m.cache" : MODELS "quill-135m.cache";
        execl("/usr/local/bin/inkllm", "inkllm", m, "/usr/local/share/quill/system.txt", c, (char *)0);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    llm_in = to[1];
    llm_out = from[0];
    fcntl(llm_out, F_SETFL, O_NONBLOCK);
    ready = busy = 0;
}

static void llm_stop(void) {
    if (llm_pid > 0) { kill(llm_pid, SIGTERM); waitpid(llm_pid, 0, 0); }
    if (llm_in >= 0) close(llm_in);
    if (llm_out >= 0) close(llm_out);
    llm_pid = 0;
    llm_in = llm_out = -1;
}

static char lbuf[8192];
static int llen;
/* One complete line from the model, unescaped. Returns 0 if none is waiting. */
static int llm_line(char *out, int n) {
    char *nl = memchr(lbuf, '\n', llen);
    if (!nl) {
        int r = read(llm_out, lbuf + llen, sizeof lbuf - 1 - llen);
        if (r > 0) llen += r;
        nl = memchr(lbuf, '\n', llen);
        if (!nl) return 0;
    }
    int len = nl - lbuf, o = 0;
    for (int i = 0; i < len && o < n - 1; i++) {
        if (lbuf[i] == '\\' && i + 1 < len) { i++; out[o++] = lbuf[i] == 'n' ? '\n' : lbuf[i]; }
        else out[o++] = lbuf[i];
    }
    out[o] = 0;
    memmove(lbuf, nl + 1, llen - len - 1);
    llen -= len + 1;
    return 1;
}

static void add_msg(int me, const char *text) {
    if (nmsgs == MAXMSG) { free(msgs[0].text); memmove(msgs, msgs + 1, sizeof *msgs * (MAXMSG - 1)); nmsgs--; }
    msgs[nmsgs].me = me;
    msgs[nmsgs].text = strdup(text);
    nmsgs++;
}
static void append_last(const char *piece) {
    msg *m = &msgs[nmsgs - 1];
    if (!m->text[0]) while (*piece == ' ' || *piece == '\n') piece++;
    size_t a = strlen(m->text), b = strlen(piece);
    m->text = realloc(m->text, a + b + 1);
    memcpy(m->text + a, piece, b + 1);
}

/* ---- layout --------------------------------------------------------------------- */

#define TOP 150
#define FIELD_H2 100
#define FIELD_Y2 (KB_TOP - 22 - FIELD_H2 - 20)
#define CHAT_B (FIELD_Y2 - 16)
#define PX 36
#define LH 50
#define BUBBLE_W (W - 2 * MARGIN - 140)

static rect new_btn, model_btn, stop_btn;

typedef struct { int count, maxw; } measure;
static void measure_line(const char *s, int n, void *v) {
    measure *m = v;
    char b[600];
    n = n < 599 ? n : 599;
    memcpy(b, s, n);
    b[n] = 0;
    int w = text_width(&f_ui, PX, b);
    if (w > m->maxw) m->maxw = w;
    m->count++;
}
typedef struct { int x, y; unsigned char ink; } drawctx;
static void draw_line_cb(const char *s, int n, void *v) {
    drawctx *d = v;
    char b[600];
    n = n < 599 ? n : 599;
    memcpy(b, s, n);
    b[n] = 0;
    if (d->y >= TOP && d->y + LH <= CHAT_B) text(&f_ui, PX, d->x, d->y, b, d->ink);
    d->y += LH;
}

/* Draws the conversation from the newest message upwards. */
static void draw_chat(void) {
    fill(0, TOP, W, CHAT_B - TOP, WHITE);
    if (!nmsgs) {
        icon("quill", W / 2, TOP + 120, 180);
        text_center(&f_serif_bold, 80, W / 2, TOP + 220, "Quill", BLACK);
        text_center(&f_ui, 32, W / 2, TOP + 330, smart ? "the smart model, slower" : "the quick model", 0x60);
        text_center(&f_ui, 28, W / 2, TOP + 380, "runs on this Kindle, nothing leaves it", 0x80);
        return;
    }
    int y = CHAT_B;
    for (int i = nmsgs - 1; i >= 0 && y > TOP; i--) {
        msg *m = &msgs[i];
        const char *t = m->text[0] ? m->text : "...";
        measure me = {0, 0};
        text_wrap(&f_ui, PX, t, BUBBLE_W - 48, measure_line, &me);
        int bh = me.count * LH + 30, bw = me.maxw + 48;
        y -= bh + 22;
        int bx = m->me ? W - MARGIN - bw : MARGIN + 50;
        if (m->me) {
            if (y >= TOP) rrect_fill(bx, y, bw, bh, 26, BLACK);
        } else {
            if (y >= TOP) {
                rrect_line(bx + 1.5f, y + 1.5f, bw - 3, bh - 3, 24, 2.5f, 0x60);
                icon_quill_small(MARGIN + 18, y + 30);
            }
        }
        drawctx d = {bx + 24, y + 12, m->me ? WHITE : BLACK};
        if (y < TOP) { /* partly visible: draw only the lines that fit */
            text_wrap(&f_ui, PX, t, BUBBLE_W - 48, draw_line_cb, &d);
            break;
        }
        text_wrap(&f_ui, PX, t, BUBBLE_W - 48, draw_line_cb, &d);
    }
}

static void draw_field(const char *buf) {
    int x = MARGIN, w = W - 2 * MARGIN;
    fill(0, FIELD_Y2 - 8, W, FIELD_H2 + 16, WHITE);
    rrect_fill(x, FIELD_Y2, w, FIELD_H2, FIELD_H2 / 2.0f, 0xEC);
    char fit[1100];
    const char *p = buf;
    while (*p && text_width(&f_ui, 38, p) > w - 90) { p++; while ((*p & 0xC0) == 0x80) p++; }
    snprintf(fit, sizeof fit, "%s", p);
    if (!buf[0] && !busy) text(&f_ui, 38, x + 54, FIELD_Y2 + 28, ready ? "Message Quill" : "Quill is waking up...", 0x80);
    else if (busy && !buf[0]) text(&f_ui, 38, x + 54, FIELD_Y2 + 28, "Quill is writing...", 0x80);
    int end = buf[0] ? text(&f_ui, 38, x + 40, FIELD_Y2 + 28, fit, BLACK) : x + 40;
    if (!busy) fill(end + 3, FIELD_Y2 + 24, 4, 52, BLACK);
}

static void draw_header(void) {
    fill(0, 0, W, TOP, WHITE);
    float cx = back_btn.x + 52, cy = back_btn.y + back_btn.h / 2.0f;
    ring(cx, cy, 34, 3, BLACK);
    line(cx + 8, cy - 15, cx - 8, cy, 5, BLACK);
    line(cx - 8, cy, cx + 8, cy + 15, 5, BLACK);
    text(&f_serif_bold, 76, back_btn.x + 110, 38, "Quill", BLACK);
    new_btn = (rect){W - MARGIN - 200, 42, 200, 76};
    model_btn = (rect){W - MARGIN - 420, 42, 200, 76};
    stop_btn = (rect){0, 0, 0, 0};
    if (busy) { stop_btn = new_btn; pill(stop_btn, "Stop", 1); }
    else pill(new_btn, "New chat", 0);
    pill(model_btn, smart ? "smart" : "quick", 0);
    fill(MARGIN, TOP - 2, W - 2 * MARGIN, 2, BLACK);
}

/* ---- main ------------------------------------------------------------------------- */

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    signal(SIGPIPE, SIG_IGN);
    FILE *f = fopen(CONF, "r");
    if (f) { smart = fgetc(f) == 's'; fclose(f); }
    if (access(MODELS "smollm2-360m-q4_0.gguf", R_OK)) smart = 0;
    llm_start();

    char input[1024] = "", lnbuf[1024];
    kb_enter_label = "send";
    fill(0, 0, W, H, WHITE);
    draw_header();
    draw_chat();
    draw_field(input);
    kb_draw();
    flip_flash();

    int dirty = 0, chat_dirty = 0;
    for (;;) {
        if (evq_head == evq_tail) {
            struct pollfd p[3] = {{touchfd, POLLIN, 0}, {keyfd, POLLIN, 0}, {llm_out, POLLIN, 0}};
            if (poll(p, 3, dirty || chat_dirty ? 500 : -1) > 0 && (p[0].revents || p[1].revents)) input_read();
        }
        while (llm_out >= 0 && llm_line(lnbuf, sizeof lnbuf)) {
            if (!strncmp(lnbuf, "READY", 5)) {
                ready = 1;
                if (!input[0]) { draw_field(input); refresh(0, FIELD_Y2 - 8, W, FIELD_H2 + 16, WF_GL16, 0, 0); }
            } else if (!strncmp(lnbuf, "T ", 2) && busy) {
                append_last(lnbuf + 2);
                draw_chat();
                refresh(0, TOP, W, CHAT_B - TOP, WF_DU, 0, 0);
                chat_dirty = 1;
            } else if (!strncmp(lnbuf, "END", 3)) {
                if (busy) {
                    busy = 0;
                    if (nmsgs && !msgs[nmsgs - 1].text[0]) append_last("(no answer)");
                    draw_header();
                    draw_chat();
                    draw_field(input);
                    refresh(0, 0, W, FIELD_Y2 + FIELD_H2 + 8, WF_GL16, 0, 0);
                    chat_dirty = 0;
                }
            } else if (!strncmp(lnbuf, "ERR", 3)) {
                add_msg(0, "Something went wrong with the model.");
                busy = 0;
                draw_header();
                draw_chat();
                refresh(0, 0, W, CHAT_B, WF_GL16, 0, 0);
            }
        }
        event e = input_wait(0);
        if (e.type == EV_NONE) {
            if (evq_head != evq_tail) continue;
            if (dirty) { refresh(0, FIELD_Y2 - 8, W, FIELD_H2 + 16, WF_GL16, 0, 0); dirty = 0; }
            if (chat_dirty && !busy) { refresh(0, TOP, W, CHAT_B - TOP, WF_GL16, 0, 0); chat_dirty = 0; }
            continue;
        }
        if (e.type == EV_POWER) break;
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(new_btn, e.x, e.y)) press_on(new_btn);
            else if (hit(model_btn, e.x, e.y)) press_on(model_btn);
            else kb_down(e.x, e.y);
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) { press_forget(); break; }
        if (busy && hit(stop_btn, e.x, e.y)) { press_off(); dprintf(llm_in, "S\n"); continue; }
        if (hit(new_btn, e.x, e.y)) {
            press_forget();
            for (int i = 0; i < nmsgs; i++) free(msgs[i].text);
            nmsgs = 0;
            if (ready) dprintf(llm_in, "R\n");
            draw_chat();
            draw_header();
            refresh(0, 0, W, CHAT_B, WF_GC16, 0, 0);
            continue;
        }
        if (hit(model_btn, e.x, e.y)) {
            press_forget();
            if (access(MODELS "smollm2-360m-q4_0.gguf", R_OK)) continue;
            smart = !smart;
            f = fopen(CONF, "w");
            if (f) { fputs(smart ? "smart\n" : "quick\n", f); fclose(f); }
            llm_stop();
            llen = 0;
            llm_start();
            for (int i = 0; i < nmsgs; i++) free(msgs[i].text);
            nmsgs = 0;
            draw_header();
            draw_chat();
            draw_field(input);
            refresh(0, 0, W, FIELD_Y2 + FIELD_H2 + 8, WF_GC16, 0, 0);
            continue;
        }
        int c = kb_tap(e.x, e.y), len = strlen(input);
        if (c == K_NONE) continue;
        if (c == K_ENTER) {
            if (!len || busy || !ready) continue;
            add_msg(1, input);
            add_msg(0, "");
            dprintf(llm_in, "U %s\n", input);
            input[0] = 0;
            busy = 1;
            draw_header();
            draw_chat();
            draw_field(input);
            refresh(0, 0, W, FIELD_Y2 + FIELD_H2 + 8, WF_GL16, 0, 0);
            continue;
        }
        if (c == K_BACK) {
            while (len > 0 && (input[len - 1] & 0xC0) == 0x80) len--;
            if (len) input[len - 1] = 0;
        } else if (c > 0 && len < (int)sizeof input - 1) {
            input[len] = (char)c;
            input[len + 1] = 0;
        }
        draw_field(input);
        refresh(0, FIELD_Y2 - 8, W, FIELD_H2 + 16, WF_A2, 0, 0);
        dirty = 1;
    }
    llm_stop();
    return 0;
}

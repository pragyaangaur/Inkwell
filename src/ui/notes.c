/* notes: plain text notes kept as files in /mnt/us/notes, so they can be read
 * from a computer too. A list of notes, newest first, and an editor with the
 * on-screen keyboard. Notes save when you leave the editor. */
#include "ui.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define DIR_NOTES "/mnt/us/notes"
#define MAXTEXT 16384

typedef struct { char file[64], title[96], preview[160]; time_t mtime; } note;
static note notes[200];
static int nnotes, scroll;

static int by_mtime(const void *a, const void *b) {
    time_t x = ((const note *)a)->mtime, y = ((const note *)b)->mtime;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void load_list(void) {
    nnotes = 0;
    mkdir(DIR_NOTES, 0755);
    DIR *d = opendir(DIR_NOTES);
    struct dirent *e;
    while (d && (e = readdir(d)) && nnotes < 200) {
        int n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".txt")) continue;
        note *t = &notes[nnotes];
        char path[160], buf[512] = "";
        snprintf(t->file, sizeof t->file, "%s", e->d_name);
        snprintf(path, sizeof path, DIR_NOTES "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        t->mtime = st.st_mtime;
        FILE *f = fopen(path, "r");
        int len = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        buf[len] = 0;
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        snprintf(t->title, sizeof t->title, "%s", buf[0] ? buf : "Untitled");
        snprintf(t->preview, sizeof t->preview, "%s", nl ? nl + 1 : "");
        for (char *p = t->preview; *p; p++) if (*p == '\n') *p = ' ';
        nnotes++;
    }
    if (d) closedir(d);
    qsort(notes, nnotes, sizeof *notes, by_mtime);
}

/* ---- the list ------------------------------------------------------------------- */

#define LIST_Y 330
#define ROW_H 170
static rect new_btn, more_btn;

static int rows_fit(void) { return (H - LIST_Y - 60) / ROW_H; }
static rect row_rect(int i) { return (rect){MARGIN, LIST_Y + i * ROW_H, W - 2 * MARGIN, ROW_H}; }

static void draw_list(void) {
    header("Notes");
    new_btn = (rect){MARGIN, 190, 300, 100};
    pill(new_btn, "+ New note", 1);
    char count[32];
    snprintf(count, sizeof count, "%d note%s", nnotes, nnotes == 1 ? "" : "s");
    text_right(&f_ui, 34, W - MARGIN, 220, count, 0x60);
    if (!nnotes) {
        icon_page(W / 2, 640, 260);
        text_center(&f_serif_italic, 60, W / 2, 820, "An empty notebook", 0x30);
        text_center(&f_ui, 34, W / 2, 910, "Notes are saved as text files in /notes", 0x70);
    }
    int fit = rows_fit();
    for (int i = 0; i < fit && scroll + i < nnotes; i++) {
        note *t = &notes[scroll + i];
        rect r = row_rect(i);
        char buf[200], when[64];
        text_fit(&f_serif_bold, 52, t->title, r.w - 20, buf, sizeof buf);
        text(&f_serif_bold, 52, r.x + 4, r.y + 18, buf, BLACK);
        strftime(when, sizeof when, "%-d %b, %H:%M", localtime(&t->mtime));
        int wx = text(&f_ui_bold, 28, r.x + 4, r.y + 96, when, 0x50) + 24;
        text_fit(&f_ui, 30, t->preview, r.x + r.w - wx, buf, sizeof buf);
        text(&f_ui, 30, wx, r.y + 94, buf, 0x70);
        fill(r.x, r.y + r.h - 2, r.w, 2, 0xC0);
    }
    more_btn = (rect){0, 0, 0, 0};
    if (nnotes > fit) {
        more_btn = (rect){W / 2 - 160, H - 120, 320, 84};
        pill(more_btn, scroll + fit < nnotes ? "More" : "Back to top", 0);
    }
    flip();
}

/* ---- the editor ------------------------------------------------------------------ */

static char text_buf[MAXTEXT];
static int text_len;
#define ED_Y 170
#define ED_PX 40
#define ED_LINE 54
static rect del_btn;

typedef struct { int y, count, max_lines; char last[512]; } wrapctx;
static void emit_line(const char *s, int n, void *v) {
    wrapctx *c = v;
    n = n < (int)sizeof c->last - 1 ? n : (int)sizeof c->last - 1;
    memcpy(c->last, s, n);
    c->last[n] = 0;
    c->count++;
    text(&f_ui, ED_PX, MARGIN, c->y, c->last, BLACK);
    c->y += ED_LINE;
}

/* Skips the first n wrapped lines of s, wrapping the same way text_wrap does. */
static const char *skip_lines(const char *s, int n, int w) {
    while (n-- > 0 && *s) {
        const char *p = s, *brk = 0;
        int lw = 0;
        while (*p && *p != '\n') {
            const char *q = p;
            int cw = glyph_get(&f_ui, ED_PX, utf8_next(&q))->adv;
            if (lw + cw > w && p > s) break;
            if (*p == ' ') brk = p;
            lw += cw;
            p = q;
        }
        const char *end = (*p && *p != '\n' && brk) ? brk : p;
        s = end + ((*end == ' ' || *end == '\n') ? 1 : 0);
    }
    return s;
}

/* Draws the text so its end (where typing happens) is always on screen, with
 * a caret after the last character. */
static void draw_text_area(void) {
    int top = ED_Y + 10, bottom = KB_TOP - 40, max_lines = (bottom - top) / ED_LINE, w = W - 2 * MARGIN;
    fill(0, top, W, bottom - top + 10, WHITE);
    int newline_end = text_len && text_buf[text_len - 1] == '\n';
    int total = text_wrap(&f_ui, ED_PX, text_buf, w, 0, 0) + newline_end;
    const char *start = skip_lines(text_buf, total - max_lines, w);
    wrapctx c = {top, 0, max_lines, ""};
    text_wrap(&f_ui, ED_PX, start, w, emit_line, &c);
    int cx = MARGIN, cy = top;
    if (c.count) {
        cy = top + (c.count - 1) * ED_LINE;
        cx = MARGIN + text_width(&f_ui, ED_PX, c.last);
    }
    if (newline_end) { cx = MARGIN; cy += c.count ? ED_LINE : 0; }
    fill(cx + 3, cy + 4, 4, ED_LINE - 6, BLACK);
}

static void editor_frame(void) {
    header("Note");
    del_btn = (rect){W - MARGIN - 220, 40, 220, 80};
    pill(del_btn, "Delete", 0);
    kb_enter_label = "return";
    kb_shift = kb_sym = 0;
    kb_draw();
    draw_text_area();
}

static void save(const char *file) {
    char path[160];
    snprintf(path, sizeof path, DIR_NOTES "/%s", file);
    if (text_len == 0) { unlink(path); return; }
    FILE *f = fopen(path, "w");
    if (f) { fwrite(text_buf, 1, text_len, f); fclose(f); }
    sync();
}

/* Edits a note in place. A NULL file starts a new one. */
static void edit(const char *file) {
    char name[64];
    if (file) snprintf(name, sizeof name, "%s", file);
    else {
        time_t now = time(0);
        strftime(name, sizeof name, "note-%Y%m%d-%H%M%S.txt", localtime(&now));
    }
    text_len = 0;
    char path[160];
    snprintf(path, sizeof path, DIR_NOTES "/%s", name);
    FILE *f = file ? fopen(path, "r") : 0;
    if (f) { text_len = fread(text_buf, 1, MAXTEXT - 1, f); fclose(f); }
    text_buf[text_len] = 0;
    editor_frame();
    flip();
    int dirty = 0, area_top = ED_Y + 10, area_h = KB_TOP - 30 - area_top;
    for (;;) {
        event e = input_wait(dirty ? 700 : -1);
        if (e.type == EV_NONE) {
            if (dirty) { refresh(0, area_top, W, area_h, WF_GL16, 0, 0); dirty = 0; }
            continue;
        }
        if (e.type == EV_POWER) { save(name); return; }
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(del_btn, e.x, e.y)) press_on(del_btn);
            else kb_down(e.x, e.y);
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) { press_forget(); save(name); return; }
        if (hit(del_btn, e.x, e.y)) { press_forget(); text_len = 0; save(name); return; }
        int c = kb_tap(e.x, e.y);
        if (c == K_NONE) continue;
        if (c == K_BACK) {
            while (text_len > 0 && (text_buf[text_len - 1] & 0xC0) == 0x80) text_len--;
            if (text_len > 0) text_len--;
        } else if (c == K_ENTER) {
            if (text_len < MAXTEXT - 1) text_buf[text_len++] = '\n';
        } else if (c > 0 && text_len < MAXTEXT - 1) {
            text_buf[text_len++] = (char)c;
        }
        text_buf[text_len] = 0;
        draw_text_area();
        refresh(0, area_top, W, area_h, WF_A2, 0, 0);
        dirty = 1;
    }
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    load_list();
    draw_list();
    for (;;) {
        event e = input_wait(-1);
        if (e.type == EV_POWER) return 0;
        int fit = rows_fit();
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(new_btn, e.x, e.y)) press_on(new_btn);
            else if (more_btn.w && hit(more_btn, e.x, e.y)) press_on(more_btn);
            else for (int i = 0; i < fit && scroll + i < nnotes; i++) if (hit(row_rect(i), e.x, e.y)) press_on(row_rect(i));
            continue;
        }
        if (e.type != EV_TAP) continue;
        press_forget();
        if (hit(back_btn, e.x, e.y)) return 0;
        if (hit(new_btn, e.x, e.y)) { edit(0); load_list(); scroll = 0; draw_list(); continue; }
        if (more_btn.w && hit(more_btn, e.x, e.y)) { scroll = scroll + fit < nnotes ? scroll + fit : 0; draw_list(); continue; }
        int opened = 0;
        for (int i = 0; i < fit && scroll + i < nnotes; i++)
            if (hit(row_rect(i), e.x, e.y)) { edit(notes[scroll + i].file); load_list(); draw_list(); opened = 1; break; }
        if (!opened) press_off();
    }
}

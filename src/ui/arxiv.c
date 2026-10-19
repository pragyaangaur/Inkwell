/* arxiv: papers from arXiv, kept on the Kindle to read offline. Type a
 * paper number (1706.03762, or an old one like hep-th/9901001), and the app
 * fetches its title, authors and abstract from arXiv's API. Save offline
 * downloads the PDF to /mnt/us/arxiv and opens it in Books, which keeps the
 * page and bookmarks. Saved papers are listed on the first screen. */
#include "ui.h"
#include <ctype.h>
#include <strings.h>
#include <signal.h>
#include <sys/wait.h>

#define ADIR "/mnt/us/arxiv"
#define INDEX ADIR "/papers.txt"
#define XML "/tmp/inkwell-arxiv.xml"

typedef struct { char id[64], title[400], authors[400], date[16]; } paper;
static paper saved[300];
static int nsaved;

/* ---- the saved list ------------------------------------------------------------------ */

static void safe_name(const char *id, char *out, int n) {
    snprintf(out, n, "%s", id);
    for (char *c = out; *c; c++) if (*c == '/') *c = '_';
}
static void pdf_path(const char *id, char *out, int n) { char s[64]; safe_name(id, s, sizeof s); snprintf(out, n, ADIR "/%s.pdf", s); }
static void abs_path(const char *id, char *out, int n) { char s[64]; safe_name(id, s, sizeof s); snprintf(out, n, ADIR "/%s.txt", s); }

static void load_index(void) {
    char line[1400];
    nsaved = 0;
    FILE *f = fopen(INDEX, "r");
    while (f && fgets(line, sizeof line, f) && nsaved < 300) {
        line[strcspn(line, "\n")] = 0;
        char *t[4] = {line, 0, 0, 0};
        for (int i = 1; i < 4; i++) { t[i] = t[i - 1] ? strchr(t[i - 1], '\t') : 0; if (t[i]) *t[i]++ = 0; }
        if (!t[3]) continue;
        paper *p = &saved[nsaved++];
        snprintf(p->id, sizeof p->id, "%s", t[0]);
        snprintf(p->title, sizeof p->title, "%s", t[1]);
        snprintf(p->authors, sizeof p->authors, "%s", t[2]);
        snprintf(p->date, sizeof p->date, "%s", t[3]);
    }
    if (f) fclose(f);
}
static void save_index(void) {
    mkdir(ADIR, 0755);
    FILE *f = fopen(INDEX ".new", "w");
    if (!f) return;
    for (int i = 0; i < nsaved; i++) fprintf(f, "%s\t%s\t%s\t%s\n", saved[i].id, saved[i].title, saved[i].authors, saved[i].date);
    fclose(f);
    rename(INDEX ".new", INDEX);
}
static int find_saved(const char *id) {
    for (int i = 0; i < nsaved; i++) if (!strcmp(saved[i].id, id)) return i;
    return -1;
}

/* ---- talking to arXiv ------------------------------------------------------------------ */

/* Runs curl and waits. While it runs, shows how much has arrived. */
static int curl(const char *url, const char *out, const char *what) {
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1);
        dup2(null, 2);
        execlp("curl", "curl", "-sfL", "--max-time", "180", "-A", "Mozilla/5.0 (Inkwell e-reader)", "-o", out, url, (char *)0);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0;
    long shown = -1;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        struct stat sb;
        long kb = stat(out, &sb) == 0 ? (long)(sb.st_size / 1024) : 0;
        if (what && kb / 100 != shown) {
            char msg[80];
            shown = kb / 100;
            if (kb >= 100) snprintf(msg, sizeof msg, "%s %.1f MB", what, kb / 1024.0);
            else snprintf(msg, sizeof msg, "%s", what);
            toast(msg);
        }
        usleep(300000);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Turns XML entities and runs of white space into plain text. */
static void plain(const char *s, const char *e, char *out, int n) {
    int k = 0, space = 0;
    while (s < e && k < n - 4) {
        if (*s == '&') {
            const char *names[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;", "&#39;"}, chars[] = "&<>\"''";
            int done = 0;
            for (int i = 0; i < 6 && !done; i++)
                if (!strncmp(s, names[i], strlen(names[i]))) { out[k++] = chars[i]; s += strlen(names[i]); done = 1; }
            if (done) { space = 0; continue; }
        }
        if (isspace((unsigned char)*s)) { if (!space && k) out[k++] = ' '; space = 1; s++; continue; }
        space = 0;
        out[k++] = *s++;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* The text between <tag> and </tag>, searching from p. */
static const char *tag(const char *p, const char *name, char *out, int n) {
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s", name);
    snprintf(close, sizeof close, "</%s>", name);
    const char *a = strstr(p, open);
    if (!a) return 0;
    a = strchr(a, '>');
    if (!a) return 0;
    a++;
    const char *b = strstr(a, close);
    if (!b) return 0;
    plain(a, b, out, n);
    return b + strlen(close);
}

static char abstract[6000];

static int fetch_meta(const char *id, paper *p) {
    char url[200];
    snprintf(url, sizeof url, "https://export.arxiv.org/api/query?id_list=%s", id);
    unlink(XML);
    if (curl(url, XML, "Asking arXiv...")) return -1;
    FILE *f = fopen(XML, "r");
    if (!f) return -1;
    static char xml[200000];
    size_t n = fread(xml, 1, sizeof xml - 1, f);
    fclose(f);
    xml[n] = 0;
    const char *e = strstr(xml, "<entry>");
    if (!e) return -2;
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", id);
    if (!tag(e, "title", p->title, sizeof p->title) || !strcmp(p->title, "Error")) return -2;
    char pub[64] = "";
    tag(e, "published", pub, sizeof pub);
    snprintf(p->date, sizeof p->date, "%.10s", pub);
    tag(e, "summary", abstract, sizeof abstract);
    const char *q = e;
    char name[120];
    int count = 0;
    while ((q = tag(q, "name", name, sizeof name))) {
        if (count < 6) {
            size_t l = strlen(p->authors);
            snprintf(p->authors + l, sizeof p->authors - l, "%s%s", count ? ", " : "", name);
        }
        count++;
    }
    if (count > 6) { size_t l = strlen(p->authors); snprintf(p->authors + l, sizeof p->authors - l, " and %d more", count - 6); }
    return 0;
}

/* Accepts what people paste: arXiv:2401.01234v2, a URL, spaces. */
static int normalise(const char *in, char *out, int n) {
    const char *p = in;
    while (*p == ' ') p++;
    const char *slash = strstr(p, "/abs/");
    if (!slash) slash = strstr(p, "/pdf/");
    if (slash) p = slash + 5;
    if (!strncasecmp(p, "arxiv:", 6)) p += 6;
    int k = 0;
    for (; *p && *p != ' ' && k < n - 1; p++) out[k++] = *p;
    out[k] = 0;
    if (k > 4 && !strcmp(out + k - 4, ".pdf")) out[k - 4] = 0;
    int digits = 0, dot = 0, sl = 0;
    for (char *c = out; *c; c++) { if (isdigit((unsigned char)*c)) digits++; else if (*c == '.') dot++; else if (*c == '/') sl++; }
    return digits >= 7 && (dot || sl) ? 0 : -1;
}

/* ---- drawing ------------------------------------------------------------------------------ */

typedef struct { font *f; int px, x, y, lh, max, ink; int lines; } wrapctx;
static void wrap_line(const char *s, int n, void *v) {
    wrapctx *c = v;
    if (c->lines >= c->max) return;
    char b[600];
    n = n < 599 ? n : 599;
    memcpy(b, s, n);
    b[n] = 0;
    if (c->lines == c->max - 1 && s[n] && s[n + 1]) { /* the last line that fits ends in an ellipsis */
        char fit[620];
        snprintf(fit, sizeof fit, "%s ...", b);
        snprintf(b, sizeof b, "%s", fit);
    }
    text(c->f, c->px, c->x, c->y + c->lines * c->lh, b, (unsigned char)c->ink);
    c->lines++;
}
/* Word-wrapped text, at most max lines. Returns the y below it. */
static int wrapped(font *f, int px, int x, int y, int w, int lh, int max, const char *t, int ink) {
    wrapctx c = {f, px, x, y, lh, max, ink, 0};
    text_wrap(f, px, t, w, wrap_line, &c);
    return y + c.lines * lh;
}

static const rect entry_r = {MARGIN, 190, 1072 - 2 * MARGIN, 120};
#define LIST_Y 400
#define ROW_H 132

static int top;
static void draw_main(void) {
    header("arXiv");
    rrect_fill(entry_r.x, entry_r.y, entry_r.w, entry_r.h, 24, 0xF0);
    rrect_line(entry_r.x + 1.5f, entry_r.y + 1.5f, entry_r.w - 3, entry_r.h - 3, 23, 3, BLACK);
    text(&f_ui, 40, entry_r.x + 34, entry_r.y + 36, "Paper number, like 1706.03762", 0x70);
    text(&f_ui_bold, 30, MARGIN, LIST_Y - 60, nsaved ? "Saved for offline reading" : "Papers you save appear here, ready to read offline.", 0x40);
    int per = (H - LIST_Y - 140) / ROW_H;
    for (int i = top; i < nsaved && i < top + per; i++) {
        paper *p = &saved[nsaved - 1 - i]; /* newest first */
        int y = LIST_Y + (i - top) * ROW_H;
        char fit[420], sub[200], au[160];
        text_fit(&f_serif_bold, 40, p->title, W - 2 * MARGIN, fit, sizeof fit);
        text(&f_serif_bold, 40, MARGIN, y + 14, fit, BLACK);
        text_fit(&f_ui, 26, p->authors, W - 2 * MARGIN - 300, au, sizeof au);
        snprintf(sub, sizeof sub, "%s", au);
        text(&f_ui, 26, MARGIN, y + 74, sub, 0x50);
        text_right(&f_ui, 26, W - MARGIN, y + 74, p->id, 0x50);
        fill(MARGIN, y + ROW_H - 2, W - 2 * MARGIN, 1, 0xB0);
    }
    if (top > 0) pill((rect){MARGIN, H - 120, 220, 90}, "Previous", 0);
    if (top + per < nsaved) pill((rect){W - MARGIN - 220, H - 120, 220, 90}, "Next", 0);
}

/* ---- reading ------------------------------------------------------------------------------- */

static void open_reader(const paper *pp) {
    char p[200];
    pdf_path(pp->id, p, sizeof p);
    pid_t pid = fork();
    if (pid == 0) { execl("/usr/local/bin/books", "books", p, pp->title, (char *)0); _exit(127); }
    if (pid > 0) waitpid(pid, 0, 0);
    input_drain();
}

/* One paper: title, authors, date, abstract, and Save, Read or Remove. */
static void detail(paper *pp, int is_saved) {
    paper p = *pp;
    char ab[6000] = "";
    if (is_saved) {
        char ap[200];
        abs_path(p.id, ap, sizeof ap);
        FILE *f = fopen(ap, "r");
        if (f) { size_t n = fread(ab, 1, sizeof ab - 1, f); ab[n] = 0; fclose(f); }
    } else snprintf(ab, sizeof ab, "%s", abstract);
    int confirm = 0;
    for (;;) {
        rect main_b = {MARGIN, H - 140, 440, 100}, side_b = {W - MARGIN - 400, H - 140, 400, 100};
        header(p.id);
        int y = wrapped(&f_serif_bold, 54, MARGIN, 180, W - 2 * MARGIN, 62, 4, p.title, BLACK) + 18;
        y = wrapped(&f_ui, 30, MARGIN, y, W - 2 * MARGIN, 40, 3, p.authors, 0x30) + 6;
        if (p.date[0]) { text(&f_ui, 28, MARGIN, y, p.date, 0x60); y += 50; }
        fill(MARGIN, y, W - 2 * MARGIN, 2, 0xB0);
        y += 26;
        int lines = (H - 170 - y) / 42;
        wrapped(&f_serif, 34, MARGIN, y, W - 2 * MARGIN, 42, lines, ab[0] ? ab : "No abstract.", BLACK);
        pill(main_b, is_saved ? "Read" : "Save offline", 1);
        pill(side_b, is_saved ? (confirm ? "Tap to confirm" : "Remove") : "Back", 0);
        flip();
        for (;;) {
            event e = input_wait(-1);
            if (e.type == EV_POWER) { sleep_now(); continue; }
            if (e.type == EV_DOWN) {
                if (hit(back_btn, e.x, e.y)) press_on(back_btn);
                else if (hit(main_b, e.x, e.y)) press_on(main_b);
                else if (hit(side_b, e.x, e.y)) press_on(side_b);
                continue;
            }
            if (e.type != EV_TAP) continue;
            press_forget();
            if (hit(back_btn, e.x, e.y) || (!is_saved && hit(side_b, e.x, e.y))) return;
            if (hit(main_b, e.x, e.y)) {
                if (!is_saved) {
                    char url[200], out[200], part[210];
                    mkdir(ADIR, 0755);
                    pdf_path(p.id, out, sizeof out);
                    snprintf(part, sizeof part, "%s.part", out);
                    snprintf(url, sizeof url, "https://arxiv.org/pdf/%s", p.id);
                    unlink(part);
                    if (curl(url, part, "Downloading...")) {
                        unlink(part);
                        toast("The download failed. Is Wi-Fi on?");
                        sleep(2);
                        break;
                    }
                    rename(part, out);
                    char ap[200];
                    abs_path(p.id, ap, sizeof ap);
                    FILE *f = fopen(ap, "w");
                    if (f) { fputs(ab, f); fclose(f); }
                    if (nsaved < 300) saved[nsaved++] = p;
                    save_index();
                    is_saved = 1;
                }
                open_reader(&p);
                break;
            }
            if (is_saved && hit(side_b, e.x, e.y)) {
                if (!confirm) { confirm = 1; break; }
                char a[200];
                pdf_path(p.id, a, sizeof a); unlink(a);
                abs_path(p.id, a, sizeof a); unlink(a);
                int k = find_saved(p.id);
                if (k >= 0) { memmove(&saved[k], &saved[k + 1], sizeof *saved * (nsaved - k - 1)); nsaved--; }
                save_index();
                return;
            }
            press_off();
        }
    }
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    signal(SIGPIPE, SIG_IGN);
    load_index();
    for (;;) {
        int per = (H - LIST_Y - 140) / ROW_H;
        const rect prev_b = {MARGIN, H - 120, 220, 90}, next_b = {W - MARGIN - 220, H - 120, 220, 90};
        draw_main();
        flip();
        event e;
        for (;;) {
            e = input_wait(-1);
            if (e.type == EV_POWER) return 0;
            if (e.type == EV_DOWN) {
                if (hit(back_btn, e.x, e.y)) press_on(back_btn);
                else if (hit(entry_r, e.x, e.y)) press_on(entry_r);
                continue;
            }
            if (e.type == EV_TAP) break;
        }
        press_forget();
        if (hit(back_btn, e.x, e.y)) return 0;
        if (top > 0 && hit(prev_b, e.x, e.y)) { top -= per; continue; }
        if (top + per < nsaved && hit(next_b, e.x, e.y)) { top += per; continue; }
        if (hit(entry_r, e.x, e.y)) {
            char buf[80] = "", id[64];
            if (!text_entry("arXiv", "Paper number, like 1706.03762", buf, sizeof buf, 0)) continue;
            if (normalise(buf, id, sizeof id)) { toast("That doesn't look like an arXiv number."); sleep(2); continue; }
            int k = find_saved(id);
            if (k >= 0) { detail(&saved[k], 1); continue; }
            paper p;
            int r = fetch_meta(id, &p);
            if (r == -1) { toast("Couldn't reach arXiv. Is Wi-Fi on?"); sleep(2); continue; }
            if (r == -2) { toast("arXiv has no paper with that number."); sleep(2); continue; }
            detail(&p, 0);
            continue;
        }
        if (e.y >= LIST_Y && e.y < LIST_Y + per * ROW_H) {
            int i = top + (e.y - LIST_Y) / ROW_H;
            if (i < nsaved) detail(&saved[nsaved - 1 - i], 1);
        }
    }
}

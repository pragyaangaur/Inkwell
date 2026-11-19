/* dashboard: a page to leave on. A clock, the weather, the next few days of
 * your calendars, and lines and buttons from a server of your own (a small
 * Python script on your computer, tools/dashboard-server.py). While it is
 * open the Kindle never goes to sleep by itself, so it keeps itself up to
 * date; it can also be the sleep screen.
 *
 * dashboard-fetch (Python) does the fetching and leaves the result in
 * linux/dashboard.cache, which this app draws. Settings are in
 * linux/dashboard.conf on the Kindle's drive.
 * The rotate button in the top bar turns the page for a Kindle on a stand.
 * Usage: dashboard           the app
 *        dashboard --sleep   draw the sleep screen version into the page and
 *                            exit 0, or exit 3 if it isn't turned on (sleep.h) */
#include "ui.h"
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#ifndef CONF /* a host build can point these elsewhere to draw the page off the device */
#define CONF "/mnt/us/linux/dashboard.conf"
#endif
#ifndef CACHE
#define CACHE "/mnt/us/linux/dashboard.cache"
#endif
#define FETCH "/usr/local/bin/dashboard-fetch"

/* ---- settings and the cache ----------------------------------------------------- */

static int has_conf, refresh_min = 15, sleep_screen, has_ics, has_weather, has_server;

static int yes(const char *v) { return !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcmp(v, "1"); }

static void load_conf(void) {
    FILE *f = fopen(CONF, "r");
    if (!f) return;
    has_conf = 1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#') continue;
        char *v = strchr(k, '=');
        if (!v) continue;
        char *ke = v;
        *v++ = 0;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        while (*v == ' ' || *v == '\t') v++;
        v[strcspn(v, "\r\n")] = 0;
        for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if (!strcasecmp(k, "refresh_minutes")) { refresh_min = atoi(v); if (refresh_min < 1) refresh_min = 1; if (refresh_min > 1440) refresh_min = 1440; }
        else if (!strcasecmp(k, "sleep_screen")) sleep_screen = yes(v);
        else if (!strcasecmp(k, "ics") && *v) has_ics = 1;
        else if (!strcasecmp(k, "latitude") && *v) has_weather = 1;
        else if (!strcasecmp(k, "server") && *v) has_server = 1;
    }
    fclose(f);
}

typedef struct { char day[12], icon[12], lo[12], hi[12], rain[12]; } wday;
typedef struct { time_t start, end; int allday; char title[160], where[120]; } event_t;
typedef struct { char label[80], value[120]; } sline;
typedef struct { char id[48], label[64]; rect r; } sbtn;

static struct {
    time_t updated;
    char place[64];
    int has_now;
    char temp[16], icon[12], desc[48], wind[24];
    wday days[7];
    int ndays;
    event_t ev[40];
    int nev;
    char srv_title[80];
    sline lines[16];
    int nlines;
    sbtn btn[8];
    int nbtn;
    char err[4][180];
    int nerr;
} D;

/* Splits a line at tabs in place. */
static int split(char *s, char **f, int max) {
    int n = 0;
    s[strcspn(s, "\r\n")] = 0;
    while (n < max) {
        f[n++] = s;
        char *t = strchr(s, '\t');
        if (!t) break;
        *t = 0;
        s = t + 1;
    }
    return n;
}
#define CP(dst, src) snprintf(dst, sizeof dst, "%s", src)

static int load_cache(void) {
    memset(&D, 0, sizeof D);
    FILE *f = fopen(CACHE, "r");
    if (!f) return -1;
    char line[1200], *v[8];
    while (fgets(line, sizeof line, f)) {
        int n = split(line, v, 8);
        if (!strcmp(v[0], "updated") && n >= 2) D.updated = atol(v[1]);
        else if (!strcmp(v[0], "place") && n >= 2) CP(D.place, v[1]);
        else if (!strcmp(v[0], "now") && n >= 5) { D.has_now = 1; CP(D.temp, v[1]); CP(D.icon, v[2]); CP(D.desc, v[3]); CP(D.wind, v[4]); }
        else if (!strcmp(v[0], "day") && n >= 6 && D.ndays < 7) {
            wday *d = &D.days[D.ndays++];
            CP(d->day, v[1]); CP(d->icon, v[2]); CP(d->lo, v[3]); CP(d->hi, v[4]); CP(d->rain, v[5]);
        } else if (!strcmp(v[0], "event") && n >= 6 && D.nev < 40) {
            event_t *e = &D.ev[D.nev++];
            e->start = atol(v[1]); e->end = atol(v[2]); e->allday = atoi(v[3]);
            CP(e->title, v[4]); CP(e->where, v[5]);
        } else if (!strcmp(v[0], "srvtitle") && n >= 2) CP(D.srv_title, v[1]);
        else if (!strcmp(v[0], "line") && n >= 3 && D.nlines < 16) { CP(D.lines[D.nlines].label, v[1]); CP(D.lines[D.nlines].value, v[2]); D.nlines++; }
        else if (!strcmp(v[0], "button") && n >= 3 && D.nbtn < 8) { CP(D.btn[D.nbtn].id, v[1]); CP(D.btn[D.nbtn].label, v[2]); D.nbtn++; }
        else if (!strcmp(v[0], "error") && n >= 3 && D.nerr < 4) { snprintf(D.err[D.nerr], sizeof D.err[0], "%s: %s", v[1], v[2]); D.nerr++; }
    }
    fclose(f);
    return 0;
}

/* ---- fetching ------------------------------------------------------------------- */

static pid_t fetch_pid;

static void start_fetch(void) {
    if (fetch_pid > 0) return;
    fetch_pid = fork();
    if (fetch_pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); }
        execl(FETCH, "dashboard-fetch", (char *)0);
        _exit(127);
    }
    if (fetch_pid < 0) fetch_pid = 0;
}

/* 1 when a fetch that was running has just finished. */
static int fetch_done(void) {
    if (fetch_pid <= 0) return 0;
    if (waitpid(fetch_pid, 0, WNOHANG) != fetch_pid) return 0;
    fetch_pid = 0;
    return 1;
}

/* Runs one of the server's actions and returns what it said. */
static int run_action(const char *id, char *msg, int n) {
    int p[2];
    msg[0] = 0;
    if (pipe(p)) { snprintf(msg, n, "Couldn't run it"); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        execl(FETCH, "dashboard-fetch", "action", id, (char *)0);
        _exit(127);
    }
    close(p[1]);
    int len = 0, r;
    while ((r = read(p[0], msg + len, n - 1 - len)) > 0 && len < n - 1) len += r;
    msg[len] = 0;
    msg[strcspn(msg, "\n")] = 0;
    close(p[0]);
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    if (!msg[0]) snprintf(msg, n, "%s", WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "Done" : "It didn't work");
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* ---- orientation ---------------------------------------------------------------- */

/* The page can stand upright or lie on either side, for a Kindle on a stand.
 * "left" is the Kindle turned a quarter to the left (its top edge pointing
 * left), "right" a quarter to the right. The choice is kept in
 * linux/dashboard.rotation. Lying down, the page is drawn upright into a
 * page of its own (draw_begin swaps it in for the screen), then copied onto
 * the screen turned (present), and touches are turned back (map_touch). Input
 * and the sleep screen keep working on the real screen in between. */
#ifndef ROTATION
#define ROTATION "/mnt/us/linux/dashboard.rotation"
#endif
enum { ROT_UP, ROT_LEFT, ROT_RIGHT };
static const char *const rot_names[] = {"portrait", "left", "right"};
static int rot;
static unsigned char *phys_fb, *canvas;
static int PW_, PH_, PP_;

static void load_rotation(void) {
    char b[16] = "";
    FILE *f = fopen(ROTATION, "r");
    if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
    b[strcspn(b, "\r\n ")] = 0;
    rot = ROT_UP;
    for (int i = 0; i < 3; i++) if (!strcmp(b, rot_names[i])) rot = i;
}
static void save_rotation(void) {
    FILE *f = save_begin(ROTATION);
    if (f) fprintf(f, "%s\n", rot_names[rot]);
    save_end(f, ROTATION, 0);
}

/* Remembers the real screen, after fb_open. */
static int screen_init(void) {
    phys_fb = fb; PW_ = W; PH_ = H; PP_ = PITCH;
    canvas = malloc((size_t)W * H);
    return canvas ? 0 : -1;
}
/* While drawing, W, H and fb are the page's, upright or lying down. */
static void draw_begin(void) { if (rot) { fb = canvas; W = PH_; H = PW_; PITCH = W; } }
static void draw_end(void) { if (rot) { fb = phys_fb; W = PW_; H = PH_; PITCH = PP_; } }

/* A rectangle of the page as a rectangle of the screen. */
static rect to_screen(rect r) {
    if (rot == ROT_LEFT) return (rect){PW_ - (r.y + r.h), r.x, r.h, r.w};
    if (rot == ROT_RIGHT) return (rect){r.y, PH_ - (r.x + r.w), r.h, r.w};
    return r;
}
/* Copies a rectangle of the page onto the screen, turned. */
static void copy_out(rect r) {
    if (!rot) return;
    int LW = PH_, LH = PW_;
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > LW) r.w = LW - r.x;
    if (r.y + r.h > LH) r.h = LH - r.y;
    for (int ly = r.y; ly < r.y + r.h; ly++) {
        const unsigned char *src = canvas + (size_t)ly * LW;
        for (int lx = r.x; lx < r.x + r.w; lx++) {
            int px = rot == ROT_LEFT ? PW_ - 1 - ly : ly, py = rot == ROT_LEFT ? lx : PH_ - 1 - lx;
            phys_fb[(size_t)py * PP_ + px] = src[lx];
        }
    }
}
/* Shows a rectangle of the page. */
static void present(rect r, int wf) {
    copy_out(r);
    rect s = to_screen(r);
    refresh(s.x, s.y, s.w, s.h, wf, 0, 0);
}
static void present_page(int flash) {
    copy_out((rect){0, 0, rot ? PH_ : PW_, rot ? PW_ : PH_});
    if (flash) flip_flash(); else flip();
}
/* A touch on the screen as a point on the page. */
static void map_touch(int *x, int *y) {
    int tx = *x, ty = *y;
    if (rot == ROT_LEFT) { *x = ty; *y = PW_ - 1 - tx; }
    else if (rot == ROT_RIGHT) { *x = PH_ - 1 - ty; *y = tx; }
}

/* Press feedback and messages, drawn on the page and shown turned. */
static rect pressed_box;
static void dash_press(rect r) {
    draw_begin(); invert(r.x, r.y, r.w, r.h); draw_end();
    pressed_box = r;
    present(r, WF_A2);
}
static void dash_release(void) {
    if (!pressed_box.w) return;
    rect r = pressed_box;
    pressed_box.w = 0;
    draw_begin(); invert(r.x, r.y, r.w, r.h); draw_end();
    present(r, WF_GL16);
}
static void dash_toast(const char *msg) {
    draw_begin();
    int w = W - 2 * MARGIN - 60, h = 170, x = (W - w) / 2, y = H / 2 - h / 2;
    if (w > 900) { w = 900; x = (W - w) / 2; }
    rrect_fill(x + 8, y + 10, w, h, 28, 0x80);
    rrect_fill(x, y, w, h, 28, WHITE);
    rrect_line(x + 2, y + 2, w - 4, h - 4, 26, 4, BLACK);
    char fit[200];
    text_fit(&f_ui_bold, 40, msg, w - 60, fit, sizeof fit);
    text_center(&f_ui_bold, 40, W / 2, y + 62, fit, BLACK);
    draw_end();
    present((rect){x, y, w + 12, h + 14}, WF_GC16);
}

/* ---- drawing -------------------------------------------------------------------- */

#define CLOCK_PX 176
#define FC_H 150
#define FOOT_Y (H - 64)
#define ROW 50

/* Where things go, worked out for the page's shape by layout(). Upright,
 * everything is one column. Lying down, the clock, weather and forecast
 * take the left column and the calendar and your computer the right. */
static int land, top_h, lx0, lx1, rx0, rx1;
static rect refresh_r, clock_r, rotate_btn;

static void layout(void) {
    land = W > H;
    top_h = land ? 120 : 140;
    lx0 = MARGIN;
    lx1 = land ? 690 : W - MARGIN;
    rx0 = land ? 770 : MARGIN;
    rx1 = W - MARGIN;
    clock_r = land ? (rect){0, 0, lx1 + 30, top_h + 440} : (rect){0, 0, W, 408};
}

/* A circular arrow, for the rotate button. */
static void icon_rotate(float cx, float cy, float s) {
    float r = s * 0.3f, t = s * 0.07f;
    arc(cx, cy, r, t, 0.6f, 5.4f, BLACK);
    float ax = cx + r * cosf(5.4f), ay = cy + r * sinf(5.4f);
    float tri[] = {ax + s * 0.16f, ay - s * 0.02f, ax - s * 0.06f, ay - s * 0.12f, ax - s * 0.02f, ay + s * 0.14f};
    poly_fill(tri, 3, BLACK);
}

static void draw_top_bar(int asleep) {
    char buf[96], st[32];
    fill(0, 0, W, top_h, WHITE);
    int by = land ? 50 : 60;
    if (!asleep) {
        float cx = back_btn.x + 52, cy = back_btn.y + back_btn.h / 2.0f;
        ring(cx, cy, 34, 3, BLACK);
        line(cx + 8, cy - 15, cx - 8, cy, 5, BLACK);
        line(cx - 8, cy, cx + 8, cy + 15, 5, BLACK);
    }
    int pct = 0;
    char cap[8];
    board_read(BATTERY_FILE("capacity"), cap, sizeof cap);
    pct = atoi(cap);
    battery_status(st, sizeof st);
    snprintf(buf, sizeof buf, "%d%%", pct);
    text_right(&f_ui_bold, 28, W - MARGIN, by, buf, BLACK);
    int bx = W - MARGIN - text_width(&f_ui_bold, 28, buf) - 70;
    battery_glyph(bx, by + 4, 26, pct, !strcmp(st, "Charging"));
    rotate_btn = (rect){0, 0, 0, 0};
    if (!asleep) {
        rotate_btn = (rect){bx - 130, by - 34, 96, 96};
        icon_rotate(rotate_btn.x + 48, rotate_btn.y + 48, 80);
    }
    if (D.place[0]) {
        char fit[80];
        text_fit(&f_ui_bold, 30, D.place, W / 2 - 40, fit, sizeof fit);
        text(&f_ui_bold, 30, asleep ? MARGIN : back_btn.x + 140, by - 2, fit, 0x40);
    }
}

/* The clock, the date and the weather now: the part redrawn every minute.
 * Asleep, a clock would stop and tell the wrong time, so the weekday takes
 * its place. */
static void draw_clock(int asleep) {
    char hm[16], date[64];
    time_t now = time(0);
    struct tm *tm = localtime(&now);
    strftime(hm, sizeof hm, asleep ? "%a" : "%H:%M", tm);
    strftime(date, sizeof date, asleep ? "%-d %B %Y" : "%A, %-d %B", tm);
    fill(clock_r.x, top_h, clock_r.w, clock_r.h - top_h - 10, WHITE);
    text(&f_serif_bold, CLOCK_PX, lx0 - 6, top_h - 14, hm, BLACK);
    text(&f_serif_italic, 54, lx0, top_h + 196, date, 0x30);
    /* the weather now: beside the clock upright, under the date lying down */
    int wx = land ? lx0 : W / 2 + 60, wy = land ? top_h + 290 : top_h;
    if (D.has_now) {
        char line2[96], fit[96];
        snprintf(line2, sizeof line2, "%s \xC2\xB7 %s", D.desc, D.wind);
        if (land) {
            weather_icon(D.icon, wx + 60, wy + 50, 120);
            text(&f_ui_bold, 72, wx + 150, wy, D.temp, BLACK);
            text_fit(&f_ui, 30, line2, lx1 - wx - 150, fit, sizeof fit);
            text(&f_ui, 30, wx + 150, wy + 92, fit, 0x40);
        } else {
            weather_icon(D.icon, wx + 60, wy + 80, 130);
            text(&f_ui_bold, 84, wx + 140, wy + 22, D.temp, BLACK);
            text_fit(&f_ui, 30, line2, W - MARGIN - wx, fit, sizeof fit);
            text(&f_ui, 30, wx, wy + 168, fit, 0x40);
        }
    } else if (!has_weather) {
        text(&f_ui, 28, wx, wy + 60, "add latitude and longitude", 0x80);
        text(&f_ui, 28, wx, wy + 98, "for the weather", 0x80);
    }
}

static void draw_forecast(int x0, int x1, int y) {
    int n = D.ndays > 4 ? 4 : D.ndays;
    if (!n) return;
    int cw = (x1 - x0) / n;
    for (int i = 0; i < n; i++) {
        wday *d = &D.days[i];
        struct tm tm = {0};
        char name[16] = "";
        if (sscanf(d->day, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
            tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_hour = 12;
            mktime(&tm);
            strftime(name, sizeof name, i == 0 ? "Today" : "%a", &tm);
        }
        int cx = x0 + i * cw + cw / 2;
        text_center(&f_ui_bold, 28, cx, y, name, BLACK);
        weather_icon(d->icon, cx, y + 70, 64);
        char t[40];
        snprintf(t, sizeof t, "%s / %s", d->hi, d->lo);
        text_center(&f_ui, 28, cx, y + 104, t, 0x30);
        if (d->rain[0] && strcmp(d->rain, "0%")) text_center(&f_ui, 24, cx, y + 136, d->rain, 0x60); /* chance of rain */
    }
}

/* "Today", "Tomorrow" or the weekday, for the day an event starts. */
static void day_label(time_t t, char *out, int n) {
    time_t now = time(0);
    struct tm a = *localtime(&now), b = *localtime(&t);
    a.tm_hour = b.tm_hour = 12; a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
    long days = (long)((mktime(&b) - mktime(&a)) / 86400);
    if (days <= 0) snprintf(out, n, "Today");
    else if (days == 1) snprintf(out, n, "Tomorrow");
    else strftime(out, n, "%A %-d %B", &b);
}

/* The calendar in the box x0..x1, y0..y1. */
static void draw_events(int x0, int x1, int y0, int y1) {
    int y = y0, tx = x0 + 150, tw = x1 - tx;
    if (!has_ics) {
        text(&f_serif_bold, 40, x0, y, "Calendar", BLACK);
        text(&f_ui, 28, x0, y + 64, "Add one with ics = ... in linux/dashboard.conf", 0x80);
        return;
    }
    if (!D.nev) {
        text(&f_serif_bold, 40, x0, y, "Today", BLACK);
        text(&f_ui, 30, x0, y + 64, D.updated ? "Nothing on the calendar." : "Fetching...", 0x60);
        return;
    }
    char last[64] = "", lab[64];
    time_t now = time(0);
    for (int i = 0; i < D.nev; i++) {
        event_t *e = &D.ev[i];
        /* an all-day event that started earlier shows under today */
        day_label(e->allday && e->start < now ? now : e->start, lab, sizeof lab);
        int need = (strcmp(lab, last) ? 58 : 0) + ROW;
        if (y + need > y1) {
            char more[48];
            snprintf(more, sizeof more, "and %d more", D.nev - i);
            text(&f_ui, 26, x0, y1 - 34, more, 0x70);
            break;
        }
        if (strcmp(lab, last)) {
            if (last[0]) y += 8;
            text(&f_serif_bold, 40, x0, y, lab, BLACK);
            y += 58;
            snprintf(last, sizeof last, "%s", lab);
        }
        char when[24], fit[200], what[300];
        int ongoing = !e->allday && e->start <= now && e->end > now;
        if (e->allday) snprintf(when, sizeof when, "all day");
        else if (ongoing) snprintf(when, sizeof when, "now");
        else strftime(when, sizeof when, "%H:%M", localtime(&e->start));
        if (ongoing) disc(x0 + 8, y + 20, 8, BLACK);
        text(&f_ui, 30, x0 + (ongoing ? 24 : 0), y, when, 0x40);
        if (e->where[0]) snprintf(what, sizeof what, "%s \xC2\xB7 %s", e->title, e->where);
        else snprintf(what, sizeof what, "%s", e->title);
        text_fit(&f_ui_bold, 32, what, tw, fit, sizeof fit);
        /* the location in grey: draw the title, then the rest lighter */
        int tl = text_width(&f_ui_bold, 32, e->title);
        if (e->where[0] && tl < tw) {
            int end = text(&f_ui_bold, 32, tx, y - 2, e->title, BLACK);
            char rest[200];
            text_fit(&f_ui, 30, what + strlen(e->title), x1 - end, rest, sizeof rest);
            text(&f_ui, 30, end, y, rest, 0x70);
        } else {
            text(&f_ui_bold, 32, tx, y - 2, fit, BLACK);
        }
        y += ROW;
    }
}

/* Your server's lines: two columns when there is room, one when not. */
static int server_cols(int x0, int x1) { return x1 - x0 > 800 ? 2 : 1; }
static int server_height(int x0, int x1, int asleep) {
    if (!has_server) return 0;
    int nc = server_cols(x0, x1);
    int h = 70 + (D.nlines ? (D.nlines + nc - 1) / nc * 48 : 48);
    if (!asleep && D.nbtn) h += 20 + (D.nbtn + 1) / 2 * 108;
    return h + 10;
}
static void draw_server(int x0, int x1, int y, int asleep) {
    fill(x0, y, x1 - x0, 2, BLACK);
    text(&f_serif_bold, 40, x0, y + 14, D.srv_title[0] ? D.srv_title : "Your computer", BLACK);
    y += 70;
    if (!D.nlines && !D.nbtn) {
        text(&f_ui, 28, x0, y, D.updated ? "Nothing from the server yet." : "Fetching...", 0x70);
        y += 48;
    }
    int nc = server_cols(x0, x1), cw = nc == 2 ? (x1 - x0 - 40) / 2 : x1 - x0;
    for (int i = 0; i < D.nlines; i++) {
        int x = x0 + (i % nc) * (cw + 40), ly = y + i / nc * 48;
        char fit[120];
        text(&f_ui, 28, x, ly, D.lines[i].label, 0x60);
        int lw = text_width(&f_ui, 28, D.lines[i].label) + 20;
        text_fit(&f_ui_bold, 28, D.lines[i].value, cw - lw, fit, sizeof fit);
        text_right(&f_ui_bold, 28, x + cw, ly, fit, BLACK);
    }
    y += (D.nlines + nc - 1) / nc * 48;
    for (int i = 0; i < D.nbtn; i++) D.btn[i].r = (rect){0, 0, 0, 0};
    if (asleep || !D.nbtn) return;
    y += 20;
    int bw = (x1 - x0 - 40) / 2;
    for (int i = 0; i < D.nbtn; i++) {
        D.btn[i].r = (rect){x0 + (i & 1) * (bw + 40), y + i / 2 * 108, bw, 88};
        pill(D.btn[i].r, D.btn[i].label, 0);
    }
}

static void draw_footer(int asleep) {
    char buf[200], t[16];
    fill(0, FOOT_Y, W, H - FOOT_Y, WHITE);
    if (asleep) {
        time_t now = time(0);
        strftime(t, sizeof t, "%H:%M", localtime(&now));
        snprintf(buf, sizeof buf, "asleep since %s \xC2\xB7 press the power button to wake", t);
        text_center(&f_ui, 28, W / 2, FOOT_Y + 12, buf, 0x70);
        return;
    }
    if (fetch_pid > 0) snprintf(buf, sizeof buf, "Updating...");
    else if (D.nerr) snprintf(buf, sizeof buf, "%s", D.err[0]);
    else if (D.updated) { strftime(t, sizeof t, "%H:%M", localtime(&D.updated)); snprintf(buf, sizeof buf, "Updated %s \xC2\xB7 tap to update now", t); }
    else snprintf(buf, sizeof buf, "Tap to update");
    char fit[200];
    text_fit(&f_ui, 26, buf, W - 2 * MARGIN, fit, sizeof fit);
    text_center(&f_ui, 26, W / 2, FOOT_Y + 14, fit, D.nerr && fetch_pid <= 0 ? 0x30 : 0x70);
    refresh_r = (rect){0, FOOT_Y - 10, W, H - FOOT_Y + 10};
}

typedef struct { int y; unsigned char ink; } setup_ctx;
static void setup_line(const char *t, int n, void *v) {
    setup_ctx *c = v;
    char ln[300];
    snprintf(ln, sizeof ln, "%.*s", n, t);
    text(&f_ui, 34, MARGIN, c->y, ln, c->ink);
    c->y += 50;
}

/* What the page says before linux/dashboard.conf exists. */
static void draw_setup(void) {
    header("Dashboard");
    static const char *const para[] = {
        "A page to leave on: the time, the weather, your calendar, and buttons for your own computer.",
        "To set it up, copy linux/dashboard.conf.example on the Kindle's drive to linux/dashboard.conf and fill it in: "
        "a calendar link (.ics), where you are for the weather, and your server if you run one.",
        "tools/dashboard-server.py in Inkwell's repository is a server to start from. It prints the two lines to put in dashboard.conf.",
    };
    setup_ctx c = {230, BLACK};
    for (int i = 0; i < 3; i++) {
        c.ink = i ? 0x40 : BLACK;
        text_wrap(&f_ui, 34, para[i], W - 2 * MARGIN, setup_line, &c);
        c.y += 30;
    }
}

/* Draws the whole page (between draw_begin and draw_end). */
static void draw_dash(int asleep) {
    layout();
    fill(0, 0, W, H, WHITE);
    rotate_btn = (rect){0, 0, 0, 0};
    if (!has_conf) { draw_setup(); return; }
    draw_top_bar(asleep);
    draw_clock(asleep);
    int foot = FOOT_Y - 20;
    if (land) {
        /* left: clock, weather, forecast. right: calendar, your computer */
        int ry = top_h + 440;
        fill(lx0, ry, lx1 - lx0, 2, BLACK);
        draw_forecast(lx0, lx1, ry + 24);
        fill(lx1 + 40, top_h + 10, 1, foot - top_h - 10, 0xB0);
        int sh = server_height(rx0, rx1, asleep), sy = foot - sh;
        draw_events(rx0, rx1, top_h + 10, sh ? sy - 20 : foot);
        if (sh) draw_server(rx0, rx1, sy, asleep);
    } else {
        int rule = 418, fc = 440, list = fc + FC_H + 30;
        fill(MARGIN, rule, W - 2 * MARGIN, 2, BLACK);
        draw_forecast(MARGIN, W - MARGIN, fc); /* without a forecast the calendar moves up */
        int sh = server_height(MARGIN, W - MARGIN, asleep), list_top = D.ndays ? list : rule + 40;
        if (D.ndays) fill(MARGIN, list - 16, W - 2 * MARGIN, 1, 0xB0);
        int sy = foot - sh;
        draw_events(MARGIN, W - MARGIN, list_top, sh ? sy - 20 : foot);
        if (sh) draw_server(MARGIN, W - MARGIN, sy, asleep);
    }
    draw_footer(asleep);
}

/* Redraws the page and shows it. */
static void show_page(int flash) {
    draw_begin(); draw_dash(0); draw_end();
    present_page(flash);
}

/* ---- the app -------------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (fb_open() || fonts_load()) return 1;
    load_conf();
    load_rotation();
    if (screen_init()) return 1;
    if (argc > 1 && !strcmp(argv[1], "--sleep")) {
        /* Drawn by whichever app is going to sleep, which then refreshes. */
        if (!has_conf || !sleep_screen || load_cache()) return 3;
        draw_begin(); draw_dash(1); draw_end();
        copy_out((rect){0, 0, rot ? PH_ : PW_, rot ? PW_ : PH_});
        return 0;
    }
    idle_hook = 0; /* the point of this page is to stay up */
    signal(SIGCHLD, SIG_DFL);
    input_open();
    load_cache();
    show_page(1);
    if (has_conf) start_fetch();
    time_t next_fetch = time(0) + refresh_min * 60;
    long minute = time(0) / 60;
    int redraws = 0;
    for (;;) {
        time_t now = time(0);
        int ms = (int)(60 - now % 60) * 1000;
        if (fetch_pid > 0 && ms > 1000) ms = 1000;
        event e = input_wait(ms);
        now = time(0);
        if (fetch_done()) {
            load_cache();
            /* a full flash now and then clears what the minute updates leave */
            show_page(++redraws % 8 == 0);
            minute = now / 60;
        }
        if (has_conf && fetch_pid <= 0 && now >= next_fetch) {
            start_fetch();
            next_fetch = now + refresh_min * 60;
            draw_begin(); draw_footer(0); draw_end();
            present(refresh_r, WF_GL16);
        }
        if (now / 60 != minute) {
            minute = now / 60;
            if (has_conf) {
                draw_begin(); draw_top_bar(0); draw_clock(0); draw_end();
                present(clock_r, WF_GL16);
            }
        }
        if (e.type == EV_NONE) continue;
        if (e.type == EV_POWER) {
            sleep_now();
            load_cache();
            show_page(1);
            if (has_conf) { start_fetch(); next_fetch = time(0) + refresh_min * 60; }
            minute = time(0) / 60;
            continue;
        }
        map_touch(&e.x, &e.y);
        map_touch(&e.ux, &e.uy);
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) dash_press(back_btn);
            else if (rotate_btn.w && hit(rotate_btn, e.x, e.y)) dash_press(rotate_btn);
            else for (int i = 0; i < D.nbtn; i++) if (D.btn[i].r.w && hit(D.btn[i].r, e.x, e.y)) dash_press(D.btn[i].r);
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) return 0;
        if (rotate_btn.w && hit(rotate_btn, e.x, e.y)) {
            pressed_box.w = 0;
            rot = (rot + 1) % 3; /* upright, turned left, turned right */
            save_rotation();
            show_page(1);
            continue;
        }
        int b = -1;
        for (int i = 0; i < D.nbtn; i++) if (D.btn[i].r.w && hit(D.btn[i].r, e.x, e.y)) b = i;
        if (b >= 0) {
            pressed_box.w = 0;
            char msg[160];
            dash_toast("Sending...");
            run_action(D.btn[b].id, msg, sizeof msg);
            dash_toast(msg);
            sleep(2);
            start_fetch(); /* the server's lines may have changed */
            show_page(0);
            continue;
        }
        dash_release();
        draw_begin(); int foot = FOOT_Y; draw_end();
        if (has_conf && e.y >= foot - 20) {
            start_fetch();
            next_fetch = time(0) + refresh_min * 60;
            draw_begin(); draw_footer(0); draw_end();
            present(refresh_r, WF_GL16);
        }
    }
}

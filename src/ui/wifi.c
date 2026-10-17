/* wifi: scans for networks, joins them with a password typed on the
 * on-screen keyboard, forgets them, and turns Wi-Fi on or off. It drives the
 * wpa_supplicant that own-init starts, and wpa_supplicant saves joined
 * networks to /mnt/us/linux/wifi.conf. */
#include "ui.h"
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#define OFF_FLAG "/mnt/us/linux/wifi.off"
#define UDHCPC_PID "/run/udhcpc.wlan0.pid"

/* Runs argv and collects its standard output. No shell, so SSIDs and
 * passwords never need quoting. */
static int run(char *const argv[], char *out, int n) {
    int p[2];
    if (pipe(p)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    int len = 0, r;
    while ((r = read(p[0], out + len, n - 1 - len)) > 0 && len < n - 1) len += r;
    out[len] = 0;
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static char wpa_out[16384];

/* wpa_cli on wlan0 with up to four arguments. Returns its output. */
static const char *wpa(const char *a, const char *b, const char *c, const char *d) {
    char *argv[] = {"wpa_cli", "-p", "/run/wpa_supplicant", "-i", "wlan0", (char *)a, (char *)b, (char *)c, (char *)d, 0};
    if (run(argv, wpa_out, sizeof wpa_out) < 0) wpa_out[0] = 0;
    return wpa_out;
}

/* Undoes wpa_cli's printf escaping of SSIDs (\xNN, \\, \", \e, \n, \r, \t). */
static void ssid_decode(const char *in, char *out, int n) {
    int o = 0;
    while (*in && o < n - 1) {
        if (in[0] == '\\' && in[1]) {
            char c = in[1];
            if (c == 'x' && in[2] && in[3]) {
                char hex[3] = {in[2], in[3], 0};
                out[o++] = (char)strtol(hex, 0, 16);
                in += 4;
                continue;
            }
            out[o++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'e' ? 27 : c;
            in += 2;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = 0;
}

/* Looks up "key=value" in wpa_cli status output. */
static void kv(const char *text, const char *key, char *out, int n) {
    int kl = strlen(key);
    out[0] = 0;
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0) {
        if (strncmp(l, key, kl) == 0 && l[kl] == '=') {
            int i = 0;
            l += kl + 1;
            while (l[i] && l[i] != '\n' && i < n - 1) { out[i] = l[i]; i++; }
            out[i] = 0;
            return;
        }
    }
}

typedef struct { char ssid[96]; int signal, secure, saved_id; } net;
static net nets[32];
static int nnets;
static int wifi_off;

static int saved_id(const char *ssid) {
    char line[512], name[96];
    const char *p = wpa("list_networks", 0, 0, 0);
    p = strchr(p, '\n'); /* skip the column header */
    while (p && *++p) {
        int i = 0;
        while (p[i] && p[i] != '\n' && i < (int)sizeof line - 1) { line[i] = p[i]; i++; }
        line[i] = 0;
        char *t1 = strchr(line, '\t');
        if (t1) {
            char *t2 = strchr(t1 + 1, '\t');
            if (t2) *t2 = 0;
            ssid_decode(t1 + 1, name, sizeof name);
            if (strcmp(name, ssid) == 0) return atoi(line);
        }
        p = strchr(p, '\n');
    }
    return -1;
}

static int by_signal(const void *a, const void *b) { return ((const net *)b)->signal - ((const net *)a)->signal; }

/* Parses scan_results into nets, strongest first, one entry per name. */
static void read_scan(void) {
    const char *p = strchr(wpa("scan_results", 0, 0, 0), '\n');
    char line[512];
    nnets = 0;
    while (p && *++p && nnets < (int)(sizeof nets / sizeof *nets)) {
        int i = 0;
        while (p[i] && p[i] != '\n' && i < (int)sizeof line - 1) { line[i] = p[i]; i++; }
        line[i] = 0;
        p = strchr(p, '\n');
        char *f[5] = {line};
        int nf = 1;
        for (char *q = line; *q && nf < 5; q++) if (*q == '\t') { *q = 0; f[nf++] = q + 1; }
        if (nf < 5 || !f[4][0]) continue;
        net n;
        ssid_decode(f[4], n.ssid, sizeof n.ssid);
        n.signal = atoi(f[2]);
        n.secure = strstr(f[3], "WPA") || strstr(f[3], "WEP");
        int dup = 0;
        for (int k = 0; k < nnets; k++)
            if (strcmp(nets[k].ssid, n.ssid) == 0) { dup = 1; if (n.signal > nets[k].signal) nets[k].signal = n.signal; }
        if (!dup) nets[nnets++] = n;
    }
    qsort(nets, nnets, sizeof *nets, by_signal);
    for (int k = 0; k < nnets; k++) nets[k].saved_id = saved_id(nets[k].ssid);
}

/* ---- drawing ---------------------------------------------------------------- */

#define STATUS_Y 178
#define TOOLS_Y 262
#define TOOLS_H 96
#define LIST_Y 400
#define ROW_H 112

static rect scan_btn, forget_btn, power_btn;
static char cur_ssid[96], cur_ip[32], cur_state[32];

static void read_status(void) {
    const char *s = wpa("status", 0, 0, 0);
    char raw[128];
    kv(s, "wpa_state", cur_state, sizeof cur_state);
    kv(s, "ssid", raw, sizeof raw);
    ssid_decode(raw, cur_ssid, sizeof cur_ssid);
    kv(s, "ip_address", cur_ip, sizeof cur_ip);
}

static void draw_status(void) {
    char line[200], fit[200];
    if (wifi_off) snprintf(line, sizeof line, "Wi-Fi is off");
    else if (strcmp(cur_state, "COMPLETED") == 0)
        snprintf(line, sizeof line, "Connected to %s%s%s", cur_ssid, cur_ip[0] ? "   " : "", cur_ip);
    else if (cur_state[0] == 0) snprintf(line, sizeof line, "Wi-Fi isn't running");
    else snprintf(line, sizeof line, "Not connected");
    fill(0, STATUS_Y, W, 60, WHITE);
    text_fit(&f_ui, 38, line, W - 2 * MARGIN, fit, sizeof fit);
    text(&f_ui, 38, MARGIN, STATUS_Y, fit, BLACK);
}

static void draw_buttons(void) {
    int bw = (W - 2 * MARGIN - 2 * 24) / 3;
    scan_btn = (rect){MARGIN, TOOLS_Y, bw, TOOLS_H};
    forget_btn = (rect){MARGIN + bw + 24, TOOLS_Y, bw, TOOLS_H};
    power_btn = (rect){MARGIN + 2 * (bw + 24), TOOLS_Y, bw, TOOLS_H};
    pill(scan_btn, "Scan", 0);
    pill(forget_btn, "Forget", 0);
    pill(power_btn, wifi_off ? "Turn on" : "Turn off", 0);
}

static int list_rows(void) { return (H - LIST_Y - 40) / ROW_H; }

static rect row_rect(int i) { return (rect){MARGIN, LIST_Y + i * ROW_H, W - 2 * MARGIN, ROW_H}; }

static void draw_list(const char *note) {
    fill(0, LIST_Y - 20, W, H - LIST_Y + 20, WHITE);
    if (note) { text_center(&f_ui, 40, W / 2, LIST_Y + 60, note, 0x40); return; }
    if (!nnets) { text_center(&f_ui, 40, W / 2, LIST_Y + 60, "No networks found", 0x40); return; }
    for (int i = 0; i < nnets && i < list_rows(); i++) {
        rect r = row_rect(i);
        char name[120], fit[120], right[64];
        int bars = nets[i].signal > -55 ? 4 : nets[i].signal > -65 ? 3 : nets[i].signal > -75 ? 2 : 1;
        int current = strcmp(cur_state, "COMPLETED") == 0 && strcmp(cur_ssid, nets[i].ssid) == 0;
        snprintf(right, sizeof right, "%s%s", current ? "connected   " : nets[i].saved_id >= 0 ? "saved   " : "", nets[i].secure ? "locked" : "open");
        int bx = r.x + r.w - 52, by = r.y + 74;
        for (int k = 0; k < 4; k++) rrect_fill(bx + k * 13, by - 10 - k * 8, 9, 10 + k * 8, 2, k < bars ? BLACK : 0xC8);
        int rw = text_width(&f_ui, 30, right);
        snprintf(name, sizeof name, "%s", nets[i].ssid);
        text_fit(current ? &f_ui_bold : &f_ui, 44, name, r.w - rw - 110, fit, sizeof fit);
        text(current ? &f_ui_bold : &f_ui, 44, r.x + 8, r.y + 30, fit, BLACK);
        text(&f_ui, 30, bx - 24 - rw, r.y + 40, right, 0x50);
        fill(r.x, r.y + r.h - 2, r.w, 2, 0xA0);
    }
}

static void draw_page(const char *note) {
    header("Wi-Fi");
    draw_status();
    draw_buttons();
    draw_list(note);
    flip();
}

static void update_status(void) {
    read_status();
    draw_status();
    refresh(0, STATUS_Y, W, 60, WF_GC16, 0, 0);
}

/* ---- actions ------------------------------------------------------------------ */

static void do_scan(void) {
    if (wifi_off) return;
    draw_list("Scanning...");
    refresh(0, LIST_Y - 20, W, H - LIST_Y + 20, WF_GC16, 0, 0);
    wpa("scan", 0, 0, 0);
    for (int i = 0; i < 8; i++) { usleep(500000); if (i >= 3) { read_scan(); if (nnets) break; } }
    read_scan();
    read_status();
    draw_status();
    draw_list(0);
    refresh(0, STATUS_Y, W, H - STATUS_Y, WF_GC16, 0, 0);
}

static void renew_dhcp(void) {
    FILE *f = fopen(UDHCPC_PID, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    if (pid > 0) kill(pid, SIGUSR1);
}

/* Waits for the link and an address, showing progress. Returns 1 when connected. */
static int wait_connected(const char *ssid) {
    char msg[160];
    snprintf(msg, sizeof msg, "Connecting to %s...", ssid);
    toast(msg);
    for (int i = 0; i < 40; i++) {
        usleep(500000);
        read_status();
        if (strcmp(cur_state, "COMPLETED") == 0) break;
    }
    if (strcmp(cur_state, "COMPLETED") != 0) return 0;
    toast("Getting an address...");
    renew_dhcp();
    for (int i = 0; i < 30 && !cur_ip[0]; i++) { usleep(500000); read_status(); }
    return 1;
}

static void connect_to(net *n) {
    char psk[128] = "", arg[160], hex[200], id[16];
    int fresh = n->saved_id < 0;
    if (fresh && n->secure) {
        char prompt[160];
        snprintf(prompt, sizeof prompt, "Password for %s", n->ssid);
        if (!text_entry("Wi-Fi password", prompt, psk, sizeof psk, 1)) return;
        if (strlen(psk) < 8) { draw_page(0); toast("Passwords are at least 8 characters"); sleep(2); draw_page(0); return; }
    }
    draw_page(0);
    if (fresh) {
        const char *r = wpa("add_network", 0, 0, 0);
        snprintf(id, sizeof id, "%d", atoi(r));
        int k = 0;
        for (const unsigned char *s = (const unsigned char *)n->ssid; *s && k < (int)sizeof hex - 3; s++) k += sprintf(hex + k, "%02x", *s);
        wpa("set_network", id, "ssid", hex);
        if (n->secure) {
            snprintf(arg, sizeof arg, "\"%s\"", psk);
            wpa("set_network", id, "psk", arg);
        } else {
            wpa("set_network", id, "key_mgmt", "NONE");
        }
        memset(psk, 0, sizeof psk);
        memset(arg, 0, sizeof arg);
    } else {
        snprintf(id, sizeof id, "%d", n->saved_id);
    }
    wpa("select_network", id, 0, 0);
    int ok = wait_connected(n->ssid);
    if (ok) {
        wpa("enable_network", "all", 0, 0); /* select_network disables the others */
        wpa("save_config", 0, 0, 0);
    } else if (fresh) {
        wpa("remove_network", id, 0, 0);
        wpa("enable_network", "all", 0, 0);
    }
    read_scan();
    draw_page(0);
    if (!ok) { toast(fresh && n->secure ? "Couldn't connect. Wrong password?" : "Couldn't connect"); sleep(2); draw_page(0); }
}

static void forget_current(void) {
    if (strcmp(cur_state, "COMPLETED") != 0) { toast("Not connected to anything"); sleep(1); draw_page(0); return; }
    int id = saved_id(cur_ssid);
    if (id >= 0) {
        char s[16];
        snprintf(s, sizeof s, "%d", id);
        wpa("remove_network", s, 0, 0);
        wpa("save_config", 0, 0, 0);
    }
    usleep(500000);
    read_status();
    read_scan();
    draw_page(0);
}

static void toggle_power(void) {
    wifi_off = !wifi_off;
    if (wifi_off) {
        FILE *f = fopen(OFF_FLAG, "w");
        if (f) fclose(f);
        wpa("disconnect", 0, 0, 0);
        nnets = 0;
    } else {
        unlink(OFF_FLAG);
        wpa("reconnect", 0, 0, 0);
    }
    sync();
    read_status();
    draw_page(wifi_off ? "Wi-Fi is off" : 0);
    if (!wifi_off) do_scan();
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    wifi_off = access(OFF_FLAG, F_OK) == 0;
    read_status();
    read_scan(); /* whatever wpa_supplicant already knows, so the page isn't empty */
    draw_page(wifi_off ? "Wi-Fi is off" : nnets ? 0 : "Scanning...");
    if (!wifi_off) do_scan();
    time_t last = time(0);
    for (;;) {
        event e = input_wait(5000);
        if (e.type == EV_POWER) return 0;
        if (e.type == EV_NONE) {
            if (time(0) - last >= 5) { update_status(); last = time(0); }
            continue;
        }
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(scan_btn, e.x, e.y)) press_on(scan_btn);
            else if (hit(forget_btn, e.x, e.y)) press_on(forget_btn);
            else if (hit(power_btn, e.x, e.y)) press_on(power_btn);
            else for (int i = 0; i < nnets && i < list_rows(); i++)
                if (!wifi_off && hit(row_rect(i), e.x, e.y)) press_on(row_rect(i));
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) { press_forget(); return 0; }
        if (hit(scan_btn, e.x, e.y)) { press_off(); do_scan(); continue; }
        if (hit(forget_btn, e.x, e.y)) { press_forget(); forget_current(); continue; }
        if (hit(power_btn, e.x, e.y)) { press_forget(); toggle_power(); continue; }
        int done = 0;
        for (int i = 0; i < nnets && i < list_rows(); i++)
            if (!wifi_off && hit(row_rect(i), e.x, e.y)) { press_forget(); connect_to(&nets[i]); done = 1; break; }
        if (!done) press_off();
    }
}

/* sshapp: SSH over Wi-Fi. Turns the ssh-wifi server (Dropbear on port 2022)
 * on and off, keeps it running while the Kindle sleeps if asked, shows the
 * address to connect to, and sets root's password for it. The password is
 * kept only as a SHA-512 crypt hash, in /mnt/us/linux/ssh-password, and
 * ssh-wifi copies it into /etc/shadow. The SSH server on the USB link is
 * separate and still takes keys only. */
#include "ui.h"
#include <arpa/inet.h>
#include <crypt.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>

#define HELPER "/usr/local/bin/ssh-wifi"
#define PASSWORD_FILE "/mnt/us/linux/ssh-password"
#define PID_FILE "/run/dropbear-wifi.pid"
#define KEYS_FILE "/root/.ssh/authorized_keys"
#define PORT 2022

static int ip_of(const char *ifname, char *out) {
    struct ifreq r;
    int s = socket(AF_INET, SOCK_DGRAM, 0), ok = -1;
    memset(&r, 0, sizeof r);
    strncpy(r.ifr_name, ifname, IFNAMSIZ - 1);
    if (s >= 0 && ioctl(s, SIOCGIFADDR, &r) == 0) { strcpy(out, inet_ntoa(((struct sockaddr_in *)&r.ifr_addr)->sin_addr)); ok = 0; }
    if (s >= 0) close(s);
    return ok;
}

/* Runs ssh-wifi with one argument and waits for it. */
static int helper(const char *arg) {
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        execl(HELPER, "ssh-wifi", arg, (char *)0);
        _exit(127);
    }
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int running(void) {
    FILE *f = fopen(PID_FILE, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    return pid > 0 && kill(pid, 0) == 0;
}

static int has_password(void) {
    char h[8] = "";
    FILE *f = fopen(PASSWORD_FILE, "r");
    if (f) { if (!fgets(h, sizeof h, f)) h[0] = 0; fclose(f); }
    return strncmp(h, "$6$", 3) == 0;
}

/* Keys own-init copied from linux/authorized_keys. */
static int key_count(void) {
    char line[4096];
    int n = 0;
    FILE *f = fopen(KEYS_FILE, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#' && *p != '\n' && *p != '\r') n++;
    }
    if (f) fclose(f);
    return n;
}

static int flag(const char *path) { return access(path, F_OK) == 0; }
static void set_flag(const char *path, int on) {
    if (on) { FILE *f = fopen(path, "w"); if (f) fclose(f); }
    else unlink(path);
    sync();
}

/* ---- drawing ---------------------------------------------------------------- */

#define STATE_Y 190
#define CARD_Y 340
#define CARD_H 330
#define POWER_Y 710
#define AWAKE_Y 860
#define LOGIN_Y 1010
#define PASS_BTN_Y 1190

static rect power_btn, awake_btn, set_btn, remove_btn;
static int on, awake, pw, keys;
static char ip[32];

static void read_state(void) {
    on = running();
    awake = flag(SSH_AWAKE_FLAG);
    pw = has_password();
    keys = key_count();
    if (ip_of("wlan0", ip)) ip[0] = 0;
}

static void card_row(int y, const char *k, const char *v, unsigned char ink) {
    char fit[120];
    text(&f_ui, 32, MARGIN + 34, y, k, 0x60);
    text_fit(&f_ui_bold, 32, v, W - 2 * MARGIN - 300, fit, sizeof fit);
    text_right(&f_ui_bold, 32, W - MARGIN - 34, y, fit, ink);
}

static void draw_state(void) {
    fill(0, STATE_Y - 10, W, CARD_Y - STATE_Y - 10, WHITE);
    text(&f_serif_bold, 64, MARGIN, STATE_Y - 10, on ? "SSH over Wi-Fi is on" : "SSH over Wi-Fi is off", BLACK);
    const char *sub = !on ? "Turn it on to log in to this Kindle from another computer."
                    : ip[0] ? "Another computer on the same Wi-Fi can log in with:"
                            : "Waiting for Wi-Fi. Join a network in the Wi-Fi app.";
    char fit[160];
    text_fit(&f_ui, 32, sub, W - 2 * MARGIN, fit, sizeof fit);
    text(&f_ui, 32, MARGIN, STATE_Y + 82, fit, 0x40);
}

static void draw_card(void) {
    char port[16], cmd[96], fit[96];
    fill(MARGIN - 4, CARD_Y - 4, W - 2 * MARGIN + 16, CARD_H + 16, WHITE);
    rrect_line(MARGIN, CARD_Y, W - 2 * MARGIN, CARD_H, 24, 3, on ? BLACK : 0xA0);
    unsigned char ink = on ? BLACK : 0x90;
    snprintf(port, sizeof port, "%d", PORT);
    card_row(CARD_Y + 30, "Address", ip[0] ? ip : "not on Wi-Fi", ink);
    card_row(CARD_Y + 86, "Port", port, ink);
    card_row(CARD_Y + 142, "User", "root", ink);
    rrect_fill(MARGIN + 24, CARD_Y + 210, W - 2 * MARGIN - 48, 92, 16, on ? 0xE8 : 0xF4);
    snprintf(cmd, sizeof cmd, "ssh -p %d root@%s", PORT, ip[0] ? ip : "<address>");
    text_fit(&f_mono, 34, cmd, W - 2 * MARGIN - 96, fit, sizeof fit);
    text_center(&f_mono, 34, W / 2, CARD_Y + 236, fit, ink);
}

static void draw_controls(void) {
    power_btn = (rect){MARGIN, POWER_Y, W - 2 * MARGIN, 110};
    pill(power_btn, on ? "Turn off" : "Turn on", !on);

    fill(0, AWAKE_Y - 10, W, LOGIN_Y - AWAKE_Y - 10, WHITE);
    awake_btn = (rect){W - MARGIN - 190, AWAKE_Y + 4, 190, 90};
    text(&f_ui_bold, 36, MARGIN, AWAKE_Y + 6, "Keep on while asleep", BLACK);
    text(&f_ui, 26, MARGIN, AWAKE_Y + 58, "It only looks asleep, and uses more battery", 0x60);
    pill(awake_btn, awake ? "On" : "Off", awake);

    fill(0, LOGIN_Y - 10, W, H - LOGIN_Y + 10, WHITE);
    text(&f_serif_bold, 52, MARGIN, LOGIN_Y, "Logging in", BLACK);
    char buf[96];
    text(&f_ui, 32, MARGIN, LOGIN_Y + 84, "Password", 0x60);
    text_right(&f_ui_bold, 32, W - MARGIN, LOGIN_Y + 84, pw ? "set" : "not set", BLACK);
    text(&f_ui, 32, MARGIN, LOGIN_Y + 134, "Keys in linux/authorized_keys", 0x60);
    snprintf(buf, sizeof buf, "%d", keys);
    text_right(&f_ui_bold, 32, W - MARGIN, LOGIN_Y + 134, keys ? buf : "none", BLACK);

    int bw = (W - 2 * MARGIN - 40) / 2;
    set_btn = (rect){MARGIN, PASS_BTN_Y, pw ? bw : W - 2 * MARGIN, 100};
    remove_btn = pw ? (rect){MARGIN + bw + 40, PASS_BTN_Y, bw, 100} : (rect){0, 0, 0, 0};
    pill(set_btn, pw ? "Change password" : "Set a password", 0);
    if (pw) pill(remove_btn, "Remove password", 0);

    const char *hint = "Anyone on the same network can try a password, so make it long.";
    char fit[160];
    text_fit(&f_ui, 28, hint, W - 2 * MARGIN, fit, sizeof fit);
    text_center(&f_ui, 28, W / 2, H - 120, fit, 0x60);
}

static void draw_page(void) {
    header("SSH");
    draw_state();
    draw_card();
    draw_controls();
    flip();
}

/* ---- actions ------------------------------------------------------------------ */

static void message(const char *msg) {
    toast(msg);
    sleep(2);
    read_state();
    draw_page();
}

/* Asks for a new password and saves its hash. Returns 1 when saved. The
 * password is shown as it is typed, since there is no second entry to
 * catch a typo. */
static int set_password(void) {
    char a[128] = "";
    int ok = 0;
    if (!text_entry("SSH password", "A password for root, at least 8 characters", a, sizeof a, 0)) goto out;
    if (strlen(a) < 8) { draw_page(); message("Use at least 8 characters"); goto out; }

    static const char abc[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char rnd[16];
    char salt[3 + 16 + 2];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, rnd, sizeof rnd) != sizeof rnd) { if (fd >= 0) close(fd); draw_page(); message("Couldn't make a salt"); goto out; }
    close(fd);
    memcpy(salt, "$6$", 3);
    for (int i = 0; i < 16; i++) salt[3 + i] = abc[rnd[i] & 63];
    strcpy(salt + 19, "$");
    const char *hash = crypt(a, salt);
    if (!hash || strncmp(hash, "$6$", 3)) { draw_page(); message("Couldn't hash the password"); goto out; }

    FILE *f = fopen(PASSWORD_FILE ".new", "w");
    if (!f || fprintf(f, "%s\n", hash) < 0 || fflush(f) || fsync(fileno(f))) {
        if (f) fclose(f);
        draw_page();
        message("Couldn't save the password");
        goto out;
    }
    fclose(f);
    rename(PASSWORD_FILE ".new", PASSWORD_FILE);
    sync();
    helper("password");
    ok = 1;
out:
    memset(a, 0, sizeof a);
    return ok;
}

static void turn_on(void) {
    if (!has_password() && !key_count()) {
        if (!set_password()) { read_state(); draw_page(); return; }
        read_state();
        draw_page();
    }
    toast("Starting...");
    set_flag(SSH_WIFI_FLAG, 1);
    helper("start");
    for (int i = 0; i < 10 && !running(); i++) usleep(200000);
    read_state();
    draw_page();
    if (!on) { set_flag(SSH_WIFI_FLAG, 0); message("The SSH server didn't start"); }
}

static void turn_off(void) {
    set_flag(SSH_WIFI_FLAG, 0);
    helper("stop");
    read_state();
    draw_page();
}

static void remove_password(void) {
    unlink(PASSWORD_FILE);
    sync();
    helper("password");
    read_state();
    draw_page();
    message(on && !keys ? "Password removed. Nobody can log in now." : "Password removed");
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    read_state();
    draw_page();
    time_t last = time(0);
    for (;;) {
        event e = input_wait(5000);
        if (e.type == EV_POWER) return 0;
        if (e.type == EV_NONE) {
            /* Wi-Fi may join or change address while the page is open. */
            if (time(0) - last >= 5) {
                char old[32];
                int was = on;
                strcpy(old, ip);
                read_state();
                if (strcmp(old, ip) || was != on) {
                    draw_state();
                    draw_card();
                    refresh(0, STATE_Y - 10, W, CARD_Y + CARD_H + 20 - STATE_Y, WF_GC16, 0, 0);
                }
                last = time(0);
            }
            continue;
        }
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(power_btn, e.x, e.y)) press_on(power_btn);
            else if (hit(awake_btn, e.x, e.y)) press_on(awake_btn);
            else if (hit(set_btn, e.x, e.y)) press_on(set_btn);
            else if (pw && hit(remove_btn, e.x, e.y)) press_on(remove_btn);
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) { press_forget(); return 0; }
        if (hit(power_btn, e.x, e.y)) { press_forget(); if (on) turn_off(); else turn_on(); continue; }
        if (hit(awake_btn, e.x, e.y)) {
            press_forget();
            set_flag(SSH_AWAKE_FLAG, !awake);
            read_state();
            draw_controls();
            refresh(0, AWAKE_Y - 10, W, LOGIN_Y - AWAKE_Y, WF_GC16, 0, 0);
            continue;
        }
        if (hit(set_btn, e.x, e.y)) {
            press_forget();
            int ok = set_password();
            read_state();
            draw_page();
            if (ok) message("Password saved");
            continue;
        }
        if (pw && hit(remove_btn, e.x, e.y)) { press_forget(); remove_password(); continue; }
        press_off();
    }
}

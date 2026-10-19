/* quill-code: Quill in the terminal, for writing code. It talks to inkllm
 * like the Quill app does, with a coding prompt, longer answers and the
 * Qwen2.5 Coder model when it is installed. Quill's code can be run, fixed
 * from its own error output, saved, or given a file of yours to read.
 * `quill` typed in the terminal starts this too.
 * Usage: quill-code             a conversation
 *        quill-code "question"  one answer
 *        quill-code --run "question"  one answer, then run its code
 * QUILL_MODEL=quick, smart or coder picks the model. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MODELS "/mnt/us/linux/models/"
#define PROMPT "/usr/local/share/quill/code.txt"

static const struct { const char *file, *name, *cache; int ctx, max; } MODEL_LIST[] = {
    /* 0.5B with a 2048 context left so little of the 512 MB that the model's
     * pages were read back from storage for every token. 1024 fits. */
    {"qwen2.5-coder-0.5b-q4_0.gguf", "Qwen2.5 Coder 0.5B", "code-qwen.cache", 1024, 384},
    {"smollm2-360m-q4_0.gguf", "SmolLM2 360M", "code-360m.cache", 1536, 400},
    {"smollm2-135m-q4_0.gguf", "SmolLM2 135M", "code-135m.cache", 1536, 400},
};

static int to_llm = -1, from_llm = -1, model = -1;
static pid_t llm;
static volatile sig_atomic_t interrupted;

static void on_int(int s) { (void)s; interrupted = 1; }

static int start(void) {
    /* QUILL_MODEL=coder, smart or quick picks one; otherwise the best installed */
    const char *want = getenv("QUILL_MODEL");
    if (want) {
        int k = !strcmp(want, "coder") ? 0 : !strcmp(want, "smart") ? 1 : !strcmp(want, "quick") ? 2 : -1;
        char p[200];
        if (k >= 0) snprintf(p, sizeof p, MODELS "%s", MODEL_LIST[k].file);
        if (k >= 0 && access(p, R_OK) == 0) model = k;
    }
    for (int i = 0; i < 3 && model < 0; i++) {
        char p[200];
        snprintf(p, sizeof p, MODELS "%s", MODEL_LIST[i].file);
        if (access(p, R_OK) == 0) model = i;
    }
    if (model < 0) { fprintf(stderr, "No model in " MODELS ". See the README for how to add one.\n"); return -1; }
    int a[2], b[2];
    if (pipe(a) || pipe(b)) return -1;
    llm = fork();
    if (llm == 0) {
        setpgid(0, 0); /* Ctrl-C reaches quill-code, which asks the model to stop */
        dup2(a[0], 0);
        dup2(b[1], 1);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 2);
        close(a[1]);
        close(b[0]);
        char m[200], c[200], ctx[16], max[16];
        snprintf(m, sizeof m, MODELS "%s", MODEL_LIST[model].file);
        snprintf(c, sizeof c, MODELS "%s", MODEL_LIST[model].cache);
        snprintf(ctx, sizeof ctx, "%d", MODEL_LIST[model].ctx);
        snprintf(max, sizeof max, "%d", MODEL_LIST[model].max);
        setenv("INKLLM_CTX", ctx, 1);
        setenv("INKLLM_MAX", max, 1);
        setenv("INKLLM_TEMP", "0.25", 1);
        execl("/usr/local/bin/inkllm", "inkllm", m, PROMPT, c, (char *)0);
        _exit(127);
    }
    close(a[0]);
    close(b[1]);
    to_llm = a[1];
    from_llm = b[0];
    return 0;
}

static char rbuf[16384];
static int rlen;
/* One line from inkllm, unescaped. Blocks; returns 0 when it has gone. */
static int llm_line(char *out, int n) {
    for (;;) {
        char *nl = memchr(rbuf, '\n', rlen);
        if (nl) {
            int len = nl - rbuf, o = 0;
            for (int i = 0; i < len && o < n - 1; i++) {
                if (rbuf[i] == '\\' && i + 1 < len) { i++; out[o++] = rbuf[i] == 'n' ? '\n' : rbuf[i]; }
                else out[o++] = rbuf[i];
            }
            out[o] = 0;
            memmove(rbuf, nl + 1, rlen - len - 1);
            rlen -= len + 1;
            return 1;
        }
        struct pollfd p = {from_llm, POLLIN, 0};
        int r = poll(&p, 1, 200);
        if (interrupted) { interrupted = 0; if (write(to_llm, "S\n", 2) < 0) return 0; }
        if (r <= 0) { if (r < 0 && errno != EINTR) return 0; continue; }
        int got = read(from_llm, rbuf + rlen, sizeof rbuf - 1 - rlen);
        if (got <= 0) return 0;
        rlen += got;
    }
}

static void send_text(const char *s) {
    char b[2];
    if (write(to_llm, "U ", 2) < 0) return;
    for (; *s; s++) {
        if (*s == '\n') { if (write(to_llm, "\\n", 2) < 0) return; }
        else if (*s == '\\') { if (write(to_llm, "\\\\", 2) < 0) return; }
        else { b[0] = *s; if (write(to_llm, b, 1) < 0) return; }
    }
    if (write(to_llm, "\n", 1) < 0) return;
}

/* ---- answers and their code --------------------------------------------------------- */

static char answer[32768];
static int alen;

/* Sends a message and prints the answer as it is written. */
static int ask(const char *msg) {
    char line[4096];
    send_text(msg);
    alen = 0;
    answer[0] = 0;
    printf("\033[1mquill>\033[0m ");
    fflush(stdout);
    while (llm_line(line, sizeof line)) {
        if (!strncmp(line, "T ", 2)) {
            fputs(line + 2, stdout);
            fflush(stdout);
            int l = strlen(line + 2);
            if (alen + l < (int)sizeof answer - 1) { memcpy(answer + alen, line + 2, l); alen += l; answer[alen] = 0; }
        } else if (!strncmp(line, "ERR", 3)) {
            printf("[%s]", line + 4);
        } else if (!strncmp(line, "END", 3)) {
            printf("\n");
            return 0;
        }
    }
    printf("\n[the model stopped]\n");
    return -1;
}

static char code[32768], lang[32];

/* The last fenced block in the answer, even if the answer ran out before
 * the closing fence. Returns 0 if there is none. */
static int last_code(void) {
    const char *p = answer, *best = 0;
    code[0] = lang[0] = 0;
    while ((p = strstr(p, "```"))) {
        const char *body = strchr(p, '\n');
        if (!body) break;
        const char *end = strstr(body + 1, "```");
        best = p;
        p = end ? end + 3 : body + strlen(body);
    }
    if (!best) return 0;
    int k = 0;
    for (const char *q = best + 3; *q && *q != '\n' && k < 31; q++) lang[k++] = (char)tolower((unsigned char)*q);
    lang[k] = 0;
    const char *body = strchr(best, '\n') + 1, *end = strstr(body, "```");
    int len = end ? (int)(end - body) : (int)strlen(body);
    snprintf(code, sizeof code, "%.*s", len, body);
    return 1;
}

static int is_python(void) {
    if (strstr(lang, "py")) return 1;
    if (strstr(lang, "sh") || strstr(lang, "bash")) return 0;
    return strstr(code, "print(") || strstr(code, "def ") || strstr(code, "import ");
}

static char last_out[8192];
static int last_status;

static void run_code(void) {
    if (!last_code()) { printf("There is no code in the last answer.\n"); return; }
    const char *path = is_python() ? "/tmp/quill-run.py" : "/tmp/quill-run.sh";
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fputs(code, f);
    fclose(f);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s %s 2>&1", is_python() ? "python3" : "sh", path);
    printf("\033[1m-- running with %s --\033[0m\n", is_python() ? "python3" : "sh");
    fflush(stdout);
    FILE *p = popen(cmd, "r");
    int k = 0;
    char buf[512];
    while (p && fgets(buf, sizeof buf, p)) {
        fputs(buf, stdout);
        fflush(stdout);
        int l = strlen(buf);
        if (k + l < (int)sizeof last_out - 1) { memcpy(last_out + k, buf, l); k += l; }
        else { /* keep the end, where errors are */
            memmove(last_out, last_out + l, k - l);
            memcpy(last_out + k - l, buf, l);
        }
    }
    last_out[k] = 0;
    int st = p ? pclose(p) : -1;
    last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    printf("\033[1m-- exit %d --\033[0m%s\n", last_status, last_status ? "  (/fix asks Quill to fix it)" : "");
}

static void save_code(const char *name) {
    if (!*name) { printf("Usage: /save file\n"); return; }
    if (!last_code()) { printf("There is no code in the last answer.\n"); return; }
    FILE *f = fopen(name, "w");
    if (!f) { perror(name); return; }
    fputs(code, f);
    fclose(f);
    printf("Saved %d bytes to %s.\n", (int)strlen(code), name);
}

static char attached[8192];

static void read_file(const char *name) {
    if (!*name) { printf("Usage: /read file\n"); return; }
    FILE *f = fopen(name, "r");
    if (!f) { perror(name); return; }
    char body[6000];
    size_t n = fread(body, 1, sizeof body - 1, f);
    body[n] = 0;
    fclose(f);
    snprintf(attached, sizeof attached, "Here is my file %s:\n```\n%s\n```\n", name, body);
    printf("Quill will see %s (%d bytes) with your next message.\n", name, (int)n);
}

static void help(void) {
    printf("Ask for code in plain words. Commands:\n"
           "  /run          run the last code block (python3 or sh)\n"
           "  /fix          send the last run's error back and ask for a fix\n"
           "  /save FILE    save the last code block\n"
           "  /read FILE    show Quill a file with your next message\n"
           "  /new          forget this conversation\n"
           "  /quit         leave (Ctrl-D works too). Ctrl-C stops an answer.\n");
}

int main(int argc, char **argv) {
    int run_after = argc > 1 && !strcmp(argv[1], "--run");
    if (run_after) { argv++; argc--; }
    signal(SIGINT, on_int);
    signal(SIGPIPE, SIG_IGN);
    if (start()) return 1;
    char line[4096];
    if (argc < 2) printf("Waking up %s. The first start takes a little longer.\n", MODEL_LIST[model].name);
    fflush(stdout);
    while (llm_line(line, sizeof line) && strncmp(line, "READY", 5))
        if (!strncmp(line, "ERR", 3)) { fprintf(stderr, "%s\n", line + 4); return 1; }
    if (argc >= 2) {
        char q[4096] = "";
        for (int i = 1; i < argc; i++) { strncat(q, argv[i], sizeof q - strlen(q) - 2); strcat(q, " "); }
        if (!ask(q) && run_after) run_code();
    } else {
        printf("Ready. /help lists the commands.\n");
        for (;;) {
            printf("\033[1myou>\033[0m ");
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin)) {
                if (errno == EINTR && interrupted) { interrupted = 0; clearerr(stdin); printf("\n"); continue; }
                printf("\n");
                break;
            }
            line[strcspn(line, "\n")] = 0;
            char *c = line;
            while (*c == ' ') c++;
            if (!*c) continue;
            if (!strcmp(c, "/quit") || !strcmp(c, "/exit")) break;
            if (!strcmp(c, "/help")) { help(); continue; }
            if (!strcmp(c, "/run")) { run_code(); continue; }
            if (!strncmp(c, "/save", 5)) { char *a = c + 5; while (*a == ' ') a++; save_code(a); continue; }
            if (!strncmp(c, "/read", 5)) { char *a = c + 5; while (*a == ' ') a++; read_file(a); continue; }
            if (!strcmp(c, "/model")) { printf("%s\n", MODEL_LIST[model].name); continue; }
            if (!strcmp(c, "/new")) {
                if (write(to_llm, "R\n", 2) < 0) break;
                while (llm_line(line, sizeof line) && strncmp(line, "END", 3)) {}
                attached[0] = 0;
                printf("A new conversation.\n");
                continue;
            }
            static char msg[16384];
            if (!strcmp(c, "/fix")) {
                if (!last_code()) { printf("There is no code to fix.\n"); continue; }
                snprintf(msg, sizeof msg, "I ran this code:\n```%s\n%s```\nIt printed:\n```\n%s\n```\nFix the code and show the whole corrected version.",
                         lang, code, last_out[0] ? last_out : "(nothing)");
            } else {
                snprintf(msg, sizeof msg, "%s%s", attached, c);
                attached[0] = 0;
            }
            if (ask(msg)) break;
        }
    }
    close(to_llm);
    kill(llm, SIGTERM);
    waitpid(llm, 0, 0);
    return 0;
}

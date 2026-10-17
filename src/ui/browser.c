/* browser: a reading browser for e-ink. Pages are fetched with curl, boiled
 * down to headings, paragraphs, lists and links, and laid out in pages you
 * turn instead of scroll. Search goes through Bing (the plain HTML engines
 * that don't challenge a non-browser), shown as a clean list of results. */
#include "ui.h"
#include <ctype.h>
#include <sys/wait.h>
#include <time.h>

#define PAGE_FILE "/tmp/browser-page.html"
#define UA "Mozilla/5.0 (X11; Linux armv7l) AppleWebKit/537.36 (KHTML, like Gecko) Inkwell/1.0"
#define MAXDOC (3 << 20)

/* ---- fetching ------------------------------------------------------------------- */

static char cur_url[2048], cur_type[128], cur_title[256];

/* Fetches url into PAGE_FILE, following redirects. Fills cur_url with where it
 * ended up. Returns 0 on success. */
static int fetch(const char *url) {
    int p[2];
    if (pipe(p)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 2);
        execlp("curl", "curl", "-sL", "--compressed", "--max-time", "25", "--max-filesize", "3000000", "-A", UA,
               "-o", PAGE_FILE, "-w", "%{url_effective}\n%{content_type}\n%{http_code}", url, (char *)0);
        _exit(127);
    }
    close(p[1]);
    char out[2400];
    int len = 0, r;
    while ((r = read(p[0], out + len, sizeof out - 1 - len)) > 0) len += r;
    out[len] = 0;
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
    char *a = strtok(out, "\n"), *b = strtok(0, "\n");
    snprintf(cur_url, sizeof cur_url, "%s", a ? a : url);
    snprintf(cur_type, sizeof cur_type, "%s", b ? b : "");
    return 0;
}

/* ---- urls --------------------------------------------------------------------------- */

static void url_host(const char *url, char *out, int n) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    int i = 0;
    while (p[i] && p[i] != '/' && p[i] != '?' && p[i] != '#' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
    if (!strncmp(out, "www.", 4)) memmove(out, out + 4, strlen(out + 4) + 1);
}

/* Resolves href against base the way a browser would. */
static void resolve(const char *base, const char *href, char *out, int n) {
    while (isspace((unsigned char)*href)) href++;
    if (strstr(href, "://") && strstr(href, "://") < href + 10) { snprintf(out, n, "%s", href); return; }
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) { snprintf(out, n, "%s", href); return; }
    if (href[0] == '/' && href[1] == '/') { snprintf(out, n, "%.*s:%s", (int)(scheme_end - base), base, href); return; }
    const char *host_end = strchr(scheme_end + 3, '/');
    int host_len = host_end ? (int)(host_end - base) : (int)strlen(base);
    if (href[0] == '/') { snprintf(out, n, "%.*s%s", host_len, base, href); return; }
    if (href[0] == '?') {
        int l = strcspn(base, "?#");
        snprintf(out, n, "%.*s%s", l, base, href);
        return;
    }
    /* relative path: drop everything after the last slash of the base path */
    int l = strcspn(base, "?#");
    int cut = l;
    while (cut > host_len && base[cut - 1] != '/') cut--;
    if (cut <= host_len) { snprintf(out, n, "%.*s/%s", host_len, base, href); return; }
    snprintf(out, n, "%.*s%s", cut, base, href);
    /* fold ./ and ../ */
    char *s;
    while ((s = strstr(out, "/./"))) memmove(s, s + 2, strlen(s + 2) + 1);
    while ((s = strstr(out, "/../"))) {
        char *prev = s - 1;
        while (prev > out + host_len && *prev != '/') prev--;
        if (prev <= out + host_len) break;
        memmove(prev, s + 3, strlen(s + 3) + 1);
    }
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Bing's result links go through bing.com/ck/a with the real address in the
 * u parameter as "a1" plus base64url. Unwraps them, so we skip a page of JS. */
static void unwrap_bing(char *url, int n) {
    if (!strstr(url, "bing.com/ck/a")) return;
    char *u = strstr(url, "&u=a1");
    if (!u) u = strstr(url, "?u=a1");
    if (!u) return;
    u += 5;
    char out[2048];
    int o = 0, bits = 0, acc = 0;
    for (; *u && *u != '&' && o < (int)sizeof out - 1; u++) {
        int v = b64val(*u);
        if (v < 0) break;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (char)((acc >> bits) & 255); }
    }
    out[o] = 0;
    if (strstr(out, "://")) snprintf(url, n, "%s", out);
}

static void url_encode(const char *in, char *out, int n) {
    int o = 0;
    for (; *in && o < n - 4; in++) {
        unsigned char c = *in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else o += snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

/* ---- the document model -------------------------------------------------------------- */

enum { T_WORD, T_BLOCK, T_BR };
enum { B_P, B_H1, B_H2, B_H3, B_LI, B_PRE, B_QUOTE, B_SMALL };
enum { S_BOLD = 1, S_ITALIC = 2 };
typedef struct { char *s; unsigned char kind, block, style, space; short link; } token;

static char *textbuf;
static int textlen;
static token *toks;
static int ntoks, cap_toks;
static char **links;
static int nlinks;

static void doc_reset(void) {
    free(textbuf);
    free(toks);
    for (int i = 0; i < nlinks; i++) free(links[i]);
    free(links);
    textbuf = malloc(MAXDOC + 16);
    textlen = 0;
    cap_toks = 1 << 16;
    toks = malloc(sizeof *toks * cap_toks);
    ntoks = 0;
    links = malloc(sizeof *links * 4096);
    nlinks = 0;
    cur_title[0] = 0;
}

static int word_space; /* whether whitespace came before the word being pushed */
static void push(int kind, int block, int style, int link, const char *s, int n) {
    if (ntoks >= cap_toks) { cap_toks *= 2; toks = realloc(toks, sizeof *toks * cap_toks); }
    if (ntoks && kind != T_WORD && toks[ntoks - 1].kind == T_BLOCK) { toks[ntoks - 1].block = block; return; }
    if (!ntoks && kind != T_WORD) return;
    token *t = &toks[ntoks++];
    t->kind = kind; t->block = block; t->style = style; t->link = link;
    t->space = kind == T_WORD ? word_space : 0;
    word_space = 1;
    t->s = 0;
    if (s && textlen + n + 1 < MAXDOC) {
        t->s = textbuf + textlen;
        memcpy(t->s, s, n);
        textlen += n;
        textbuf[textlen++] = 0;
    }
}

static int add_link(const char *href) {
    if (nlinks >= 4096) return -1;
    char abs[2048];
    resolve(cur_url, href, abs, sizeof abs);
    if (!strncmp(abs, "javascript:", 11) || !strncmp(abs, "mailto:", 7)) return -1;
    links[nlinks] = strdup(abs);
    return nlinks++;
}

/* Appends the UTF-8 for code point c. */
static int put_utf8(char *o, int c) {
    if (c < 0x80) { o[0] = c; return 1; }
    if (c < 0x800) { o[0] = 0xC0 | (c >> 6); o[1] = 0x80 | (c & 63); return 2; }
    if (c < 0x10000) { o[0] = 0xE0 | (c >> 12); o[1] = 0x80 | ((c >> 6) & 63); o[2] = 0x80 | (c & 63); return 3; }
    o[0] = 0xF0 | (c >> 18); o[1] = 0x80 | ((c >> 12) & 63); o[2] = 0x80 | ((c >> 6) & 63); o[3] = 0x80 | (c & 63);
    return 4;
}

/* Decodes one entity at s (just after '&'). Returns its length, or 0. */
static int entity(const char *s, char *out, int *outn) {
    static const struct { const char *name; int c; } named[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '}, {"mdash", 0x2014}, {"ndash", 0x2013},
        {"hellip", 0x2026}, {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"copy", 0xA9}, {"middot", 0xB7},
        {"bull", 0x2022}, {"times", 0xD7}, {"deg", 0xB0}, {"eacute", 0xE9}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"rarr", 0x2192}};
    int c = -1, len = 0;
    if (s[0] == '#') {
        char *end;
        c = (s[1] == 'x' || s[1] == 'X') ? (int)strtol(s + 2, &end, 16) : (int)strtol(s + 1, &end, 10);
        len = end - s;
    } else {
        for (unsigned i = 0; i < sizeof named / sizeof *named; i++) {
            int l = strlen(named[i].name);
            if (!strncmp(s, named[i].name, l)) { c = named[i].c; len = l; break; }
        }
    }
    if (c < 0 || len == 0) return 0;
    if (s[len] == ';') len++;
    *outn = put_utf8(out, c == 0xA0 ? ' ' : c);
    return len;
}

/* Elements that never have a closing tag, so they can't open a skipped region. */
static int is_void_tag(const char *t) {
    static const char *const v[] = {"br", "img", "hr", "input", "meta", "link", "wbr", "source", "area", "col", "embed", 0};
    for (int i = 0; v[i]; i++) if (!strcmp(t, v[i])) return 1;
    return 0;
}

/* Turns HTML into tokens. Tolerant rather than correct: it tracks just
 * enough (skipped elements, block breaks, bold, italic, links, pre). */
static void parse_html(const char *h, int n) {
    int skip = 0, pre = 0, style = 0, link = -1, block = B_P, in_title = 0, space = 1, start_space = 1;
    char skip_tag[16] = "";
    char word[1024];
    int wl = 0;
#define FLUSH_WORD() do { if (wl) { word_space = start_space; push(T_WORD, block, style, link, word, wl); wl = 0; space = 0; } } while (0)
    for (int i = 0; i < n;) {
        if (h[i] == '<') {
            /* comments and doctype */
            if (!strncmp(h + i, "<!--", 4)) { const char *e = strstr(h + i + 4, "-->"); i = e ? (int)(e - h) + 3 : n; continue; }
            int j = i + 1, closing = 0;
            if (h[j] == '/') { closing = 1; j++; }
            char tag[16];
            int tl = 0;
            while (j < n && (isalnum((unsigned char)h[j])) && tl < 15) tag[tl++] = tolower((unsigned char)h[j++]);
            tag[tl] = 0;
            int end = j;
            char q = 0;
            while (end < n && (h[end] != '>' || q)) { if (h[end] == '"' || h[end] == '\'') q = q == h[end] ? 0 : q ? q : h[end]; end++; }
            const char *attrs = h + j;
            int alen = end - j;
            i = end + 1;
            if (!tl) continue;
            if (skip) {
                if (closing && !strcmp(tag, skip_tag) && --skip == 0) skip_tag[0] = 0;
                else if (!closing && !strcmp(tag, skip_tag)) skip++;
                continue;
            }
            int is_skip = 0, is_junk = 0;
            static const char *const skipped[] = {"script", "style", "noscript", "svg", "nav", "footer", "aside", "form",
                                                  "button", "select", "iframe", "template", "figure", "math", 0};
            /* clutter recognised by class or id: infoboxes, navigation, citations */
            static const char *const junk[] = {"infobox", "navbox", "sidebar", "mw-jump", "skip", "cookie", "banner", "advert",
                                               "share", "toc", "reference", "reflist", "mw-editsection", "noprint", "catlinks",
                                               "navigation", "menu", "hatnote", "metadata", "mw-empty", "printfooter", "mw-portlet", "vector-menu",
                                               "vector-dropdown", "vector-page-toolbar", "vector-header", "vector-search", "mw-indicators",
                                               "mw-footer", "vector-pinnable", "vector-sticky", "flash", "js-flash", "sr-only", "show-on-focus",
                                               "js-header-wrapper", "appheader", "visually-hidden", "js-stale-session-flash", 0};
            if (!closing && !is_void_tag(tag) && strcmp(tag, "html") && strcmp(tag, "body") && strcmp(tag, "main") &&
                strcmp(tag, "article") && strcmp(tag, "head")) {
                for (int k = 0; k + 6 < alen; k++) {
                    if (strncasecmp(attrs + k, "class=", 6) && strncasecmp(attrs + k, "id=", 3)) continue;
                    const char *v = attrs + k + (attrs[k] == 'i' || attrs[k] == 'I' ? 3 : 6);
                    char val[256];
                    int vl = 0;
                    char vq = (*v == '"' || *v == '\'') ? *v++ : 0;
                    while (v < attrs + alen && (vq ? *v != vq : !isspace((unsigned char)*v)) && vl < 255) val[vl++] = tolower((unsigned char)*v++);
                    val[vl] = 0;
                    /* whole class words only: "toc" must not match "vector-toc-available" */
                    for (char *w = strtok(val, " \t"); w && !is_junk; w = strtok(0, " \t"))
                        for (int m = 0; junk[m]; m++) {
                            int jl = strlen(junk[m]);
                            if (!strncmp(w, junk[m], jl) && (!w[jl] || w[jl] == '-' || w[jl] == '_')) { is_junk = 1; break; }
                        }
                    if (is_junk) break;
                }
            }
            for (int k = 0; skipped[k]; k++) if (!strcmp(tag, skipped[k])) is_skip = 1;
            if ((is_skip || is_junk) && !closing) {
                FLUSH_WORD();
                snprintf(skip_tag, sizeof skip_tag, "%s", tag);
                skip = 1;
                continue;
            }
            if (!strcmp(tag, "title")) { in_title = !closing; continue; }
            if (!strcmp(tag, "head") && closing) cur_title[0] = cur_title[0];
            FLUSH_WORD();
            if (!strcmp(tag, "br")) { push(T_BR, block, style, link, 0, 0); continue; }
            if (!strcmp(tag, "b") || !strcmp(tag, "strong")) { style = closing ? style & ~S_BOLD : style | S_BOLD; continue; }
            if (!strcmp(tag, "i") || !strcmp(tag, "em") || !strcmp(tag, "cite")) { style = closing ? style & ~S_ITALIC : style | S_ITALIC; continue; }
            if (!strcmp(tag, "a")) {
                if (closing) { link = -1; continue; }
                const char *hr = 0;
                for (int k = 0; k + 5 < alen; k++) if (!strncasecmp(attrs + k, "href=", 5)) { hr = attrs + k + 5; break; }
                if (hr) {
                    char href[2048];
                    int hl = 0;
                    char qq = (*hr == '"' || *hr == '\'') ? *hr++ : 0;
                    while (hr < attrs + alen && (qq ? *hr != qq : !isspace((unsigned char)*hr) && *hr != '>') && hl < 2047) {
                        if (*hr == '&') {
                            char ent[8];
                            int en = 0, el = entity(hr + 1, ent, &en);
                            if (el) { memcpy(href + hl, ent, en); hl += en; hr += el + 1; continue; }
                        }
                        href[hl++] = *hr++;
                    }
                    href[hl] = 0;
                    link = href[0] && href[0] != '#' ? add_link(href) : -1;
                }
                continue;
            }
            int nb = -1;
            if (!strcmp(tag, "h1")) nb = B_H1;
            else if (!strcmp(tag, "h2")) nb = B_H2;
            else if (tag[0] == 'h' && tag[1] >= '3' && tag[1] <= '6' && !tag[2]) nb = B_H3;
            else if (!strcmp(tag, "li") || !strcmp(tag, "dt")) nb = B_LI;
            else if (!strcmp(tag, "pre")) { pre = !closing; nb = closing ? B_P : B_PRE; }
            else if (!strcmp(tag, "blockquote")) nb = closing ? B_P : B_QUOTE;
            else if (!strcmp(tag, "ul") || !strcmp(tag, "ol")) nb = B_P;
            else if (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "tr") || !strcmp(tag, "section") || !strcmp(tag, "article") ||
                     !strcmp(tag, "dd") || !strcmp(tag, "table") || !strcmp(tag, "figcaption") || !strcmp(tag, "main") || !strcmp(tag, "hr"))
                nb = B_P;
            if (nb >= 0) {
                if (closing && (nb == B_H1 || nb == B_H2 || nb == B_H3 || nb == B_LI)) nb = B_P;
                if (nb == B_P && block == B_QUOTE && !closing) nb = B_QUOTE;
                block = nb;
                push(T_BLOCK, block, 0, -1, 0, 0);
                space = 1;
            }
            continue;
        }
        /* text */
        char out[8];
        int on = 1;
        if (h[i] == '&') {
            int el = entity(h + i + 1, out, &on);
            if (el) i += el + 1;
            else { out[0] = '&'; on = 1; i++; }
        } else {
            out[0] = h[i++];
        }
        if (skip) continue;
        if (in_title) {
            int tl = strlen(cur_title);
            if (tl + on < (int)sizeof cur_title - 1 && !(on == 1 && isspace((unsigned char)out[0]) && (!tl || cur_title[tl - 1] == ' '))) {
                memcpy(cur_title + tl, on == 1 && isspace((unsigned char)out[0]) ? " " : out, on);
                cur_title[tl + on] = 0;
            }
            continue;
        }
        if (on == 1 && isspace((unsigned char)out[0])) {
            if (pre && out[0] == '\n') { FLUSH_WORD(); push(T_BR, block, style, link, 0, 0); space = 1; continue; }
            FLUSH_WORD();
            space = 1;
            continue;
        }
        if (!wl) start_space = space;
        if (wl + on < (int)sizeof word) { memcpy(word + wl, out, on); wl += on; }
    }
    FLUSH_WORD();
#undef FLUSH_WORD
}

/* Bing's results page, rebuilt as title, address and snippet for each result. */
static void parse_bing(const char *h) {
    const char *p = h;
    snprintf(cur_title, sizeof cur_title, "Search results");
    while ((p = strstr(p, "<li class=\"b_algo\""))) {
        const char *next = strstr(p + 10, "<li class=\"b_algo\"");
        const char *h2 = strstr(p, "<h2");
        if (!h2 || (next && h2 > next)) { p += 10; continue; }
        const char *a = strstr(h2, "<a ");
        const char *h2e = strstr(h2, "</h2>");
        if (!a || !h2e) break;
        const char *snip = strstr(h2e, "<p");
        const char *snipe = snip ? strstr(snip, "</p>") : 0;
        if (next && snip && snip > next) snip = 0;
        /* reuse the generic parser on the fragments */
        push(T_BLOCK, B_H3, 0, -1, 0, 0);
        parse_html(h2, (int)(h2e - h2) + 5);
        if (ntoks && toks[ntoks - 1].kind == T_BLOCK) toks[ntoks - 1].block = B_H3;
        int title_link = -1;
        int k = ntoks - 1;
        while (k >= 0 && toks[k].kind != T_WORD) k--;
        for (; k >= 0 && toks[k].kind == T_WORD; k--) if (toks[k].link >= 0) title_link = toks[k].link;
        if (title_link >= 0) {
            unwrap_bing(links[title_link], 2048);
            char host[256];
            url_host(links[title_link], host, sizeof host);
            push(T_BLOCK, B_SMALL, 0, -1, 0, 0);
            push(T_WORD, B_SMALL, 0, -1, host, strlen(host));
        }
        if (snip && snipe) {
            push(T_BLOCK, B_P, 0, -1, 0, 0);
            parse_html(snip, (int)(snipe - snip) + 4);
        }
        p = next ? next : p + 10;
        if (!next) break;
    }
    if (!ntoks) push(T_WORD, B_P, 0, -1, "No results.", 11);
}

static void parse_plain(const char *s, int n) {
    int start = -1;
    for (int i = 0; i <= n; i++) {
        if (i == n || isspace((unsigned char)s[i])) {
            if (start >= 0) push(T_WORD, B_PRE, 0, -1, s + start, i - start);
            start = -1;
            if (i < n && s[i] == '\n') push(T_BR, B_PRE, 0, -1, 0, 0);
        } else if (start < 0) start = i;
    }
}

/* Pushes a sentence as paragraph words. */
static void parse_plain_words(const char *s) {
    while (*s) {
        const char *e = strchr(s, ' ');
        int n = e ? (int)(e - s) : (int)strlen(s);
        push(T_WORD, B_P, 0, -1, s, n);
        s += n;
        while (*s == ' ') s++;
    }
}

static int load_document(void) {
    FILE *f = fopen(PAGE_FILE, "rb");
    if (!f) return -1;
    char *h = malloc(MAXDOC + 1);
    int n = fread(h, 1, MAXDOC, f);
    fclose(f);
    h[n] = 0;
    doc_reset();
    char host[256];
    url_host(cur_url, host, sizeof host);
    if (strstr(cur_type, "html") || !cur_type[0]) {
        if (strstr(host, "bing.com") && strstr(cur_url, "/search")) parse_bing(h);
        else parse_html(h, n);
    } else if (!strncmp(cur_type, "text/", 5)) {
        parse_plain(h, n);
    } else {
        const char *msg = "This is not a page Inkwell can show.";
        push(T_WORD, B_P, 0, -1, msg, strlen(msg));
    }
    free(h);
    int words = 0;
    for (int i = 0; i < ntoks && !words; i++) words = toks[i].kind == T_WORD;
    if (!words) {
        const char *msg[] = {"Nothing to read here.", "This site sends a page that only works with JavaScript, or it blocks simple browsers like this one. LinkedIn does both."};
        push(T_BLOCK, B_H2, 0, -1, 0, 0);
        push(T_WORD, B_H2, 0, -1, msg[0], strlen(msg[0]));
        push(T_BLOCK, B_P, 0, -1, 0, 0);
        parse_plain_words(msg[1]);
    }
    if (!cur_title[0]) snprintf(cur_title, sizeof cur_title, "%s", host);
    return 0;
}

/* ---- layout ------------------------------------------------------------------------- */

#define TOP_H 160
#define BOTTOM_H 120
#define CONTENT_Y (TOP_H + 24)
#define CONTENT_B (H - BOTTOM_H - 16)
#define CONTENT_X MARGIN
#define CONTENT_W (W - 2 * MARGIN)

typedef struct { int first_tok, page; } linemark;
static int *page_start, npages, page_cap, cur_page;

static font *tok_font(const token *t, int *px) {
    switch (t->block) {
    case B_H1: *px = 66; return &f_serif_bold;
    case B_H2: *px = 56; return &f_serif_bold;
    case B_H3: *px = 48; return &f_serif_bold;
    case B_PRE: *px = 28; return &f_mono;
    case B_SMALL: *px = 28; return &f_ui;
    default:
        *px = 46;
        return t->style & S_BOLD ? &f_serif_bold : t->style & S_ITALIC ? &f_serif_italic : &f_serif;
    }
}

typedef struct { rect r; int link; } linkbox;
static linkbox boxes[600];
static int nboxes;

/* Lays the document out from token `from`, drawing if draw is set, and stops
 * at the bottom of one page. Returns the first token of the next page. */
static int layout_page(int from, int draw) {
    int y = CONTENT_Y, i = from;
    nboxes = 0;
    while (i < ntoks) {
        /* gather one line */
        if (toks[i].kind == T_BLOCK) {
            if (y > CONTENT_Y) y += toks[i].block <= B_H3 && toks[i].block != B_P ? 34 : 20;
            i++;
            continue;
        }
        if (toks[i].kind == T_BR) { if (y > CONTENT_Y) y += 10; i++; }
        int px, maxpx = 0, x = 0, j = i, indent = 0;
        int blk = i < ntoks ? toks[i].block : B_P;
        if (blk == B_LI) indent = 44;
        if (blk == B_QUOTE) indent = 36;
        int line_w = CONTENT_W - indent;
        while (j < ntoks && toks[j].kind == T_WORD) {
            font *f = tok_font(&toks[j], &px);
            int ww = text_width(f, px, toks[j].s), sp = j > i && toks[j].space ? glyph_get(f, px, ' ')->adv : 0;
            if (x + sp + ww > line_w && j > i) break;
            x += sp + ww;
            if (px > maxpx) maxpx = px;
            j++;
            if (ww > line_w) break;
        }
        if (j == i) { i++; continue; }
        int lh = maxpx * 135 / 100;
        if (y + lh > CONTENT_B && y > CONTENT_Y) return i;
        if (draw) {
            int cx = CONTENT_X + indent;
            if (blk == B_LI && (i == 0 || toks[i - 1].kind == T_BLOCK)) disc(CONTENT_X + 14, y + maxpx * 0.62f, 6, BLACK);
            if (blk == B_QUOTE) fill(CONTENT_X + 8, y, 4, lh, 0x90);
            for (int k = i; k < j; k++) {
                font *f = tok_font(&toks[k], &px);
                int gap = k > i && toks[k].space ? glyph_get(f, px, ' ')->adv : 0;
                /* carry a link's underline across the gap between its words */
                if (gap && toks[k].link >= 0 && toks[k - 1].link == toks[k].link) fill(cx, y + (maxpx - px) * 3 / 4 + px + 2, gap, 2, 0x40);
                cx += gap;
                int base_y = y + (maxpx - px) * 3 / 4;
                unsigned char ink = toks[k].block == B_SMALL ? 0x60 : BLACK;
                int ex;
                if (text_width(f, px, toks[k].s) > CONTENT_X + CONTENT_W - cx) {
                    char cut[512];
                    text_fit(f, px, toks[k].s, CONTENT_X + CONTENT_W - cx, cut, sizeof cut);
                    ex = text(f, px, cx, base_y, cut, ink);
                } else {
                    ex = text(f, px, cx, base_y, toks[k].s, ink);
                }
                if (toks[k].link >= 0) {
                    fill(cx, base_y + px + 2, ex - cx, 2, 0x40);
                    /* extend the previous box if it is the same link on this line */
                    if (nboxes && boxes[nboxes - 1].link == toks[k].link && boxes[nboxes - 1].r.y == y)
                        boxes[nboxes - 1].r.w = ex - boxes[nboxes - 1].r.x;
                    else if (nboxes < 600)
                        boxes[nboxes++] = (linkbox){{cx, y, ex - cx, lh}, toks[k].link};
                }
                cx = ex;
            }
        }
        y += lh;
        i = j;
    }
    return ntoks;
}

static void paginate(void) {
    npages = 0;
    int i = 0;
    do {
        if (npages >= page_cap) { page_cap = page_cap ? page_cap * 2 : 64; page_start = realloc(page_start, sizeof *page_start * page_cap); }
        page_start[npages++] = i;
        i = layout_page(i, 0);
    } while (i < ntoks && npages < 2000);
}

/* ---- screens ------------------------------------------------------------------------ */

static rect addr_btn, hist_btn, prev_btn, next_btn;
static char history[32][2048];
static int hist_n;

static void draw_top(void) {
    fill(0, 0, W, TOP_H, WHITE);
    float cx = back_btn.x + 52, cy = 80;
    ring(cx, cy, 34, 3, BLACK);
    line(cx + 8, cy - 15, cx - 8, cy, 5, BLACK);
    line(cx - 8, cy, cx + 8, cy + 15, 5, BLACK);
    hist_btn = (rect){150, 34, 110, 92};
    if (hist_n > 1) {
        rrect_line(hist_btn.x + 10, hist_btn.y + 6, 90, 80, 24, 3, BLACK);
        arc(hist_btn.x + 55, hist_btn.y + 50, 20, 4, 3.4f, 7.5f, BLACK);
        float a[] = {hist_btn.x + 30, hist_btn.y + 38, hist_btn.x + 46, hist_btn.y + 46, hist_btn.x + 30, hist_btn.y + 58};
        poly_fill(a, 3, BLACK);
    }
    addr_btn = (rect){280, 34, W - 280 - MARGIN, 92};
    rrect_fill(addr_btn.x, addr_btn.y, addr_btn.w, addr_btn.h, 46, 0xE8);
    char host[256], fit[300];
    if (cur_url[0]) url_host(cur_url, host, sizeof host);
    else snprintf(host, sizeof host, "Search or type an address");
    text_fit(&f_ui_bold, 34, host, addr_btn.w - 60, fit, sizeof fit);
    text(&f_ui_bold, 34, addr_btn.x + 34, addr_btn.y + 24, fit, cur_url[0] ? BLACK : 0x70);
    fill(MARGIN, TOP_H - 2, W - 2 * MARGIN, 2, BLACK);
}

static void draw_bottom(void) {
    int y = H - BOTTOM_H;
    fill(0, y, W, BOTTOM_H, WHITE);
    fill(MARGIN, y, W - 2 * MARGIN, 2, 0xA0);
    prev_btn = (rect){MARGIN, y + 18, 240, 84};
    next_btn = (rect){W - MARGIN - 240, y + 18, 240, 84};
    if (cur_page > 0) pill(prev_btn, "< Prev", 0);
    if (cur_page < npages - 1) pill(next_btn, "Next >", 1);
    char buf[64], fit[300];
    snprintf(buf, sizeof buf, "%d / %d", cur_page + 1, npages);
    text_center(&f_ui_bold, 30, W / 2, y + 26, buf, BLACK);
    text_fit(&f_ui, 24, cur_title, 420, fit, sizeof fit);
    text_center(&f_ui, 24, W / 2, y + 68, fit, 0x70);
}

static void draw_page(int flash) {
    fill(0, 0, W, H, WHITE);
    draw_top();
    layout_page(page_start[cur_page], 1);
    draw_bottom();
    if (flash) flip_flash(); else flip();
}

static const char *const bookmarks[][2] = {
    {"GitHub", "https://github.com/pragyaangaur"},
    {"LinkedIn", "https://www.linkedin.com/in/pragyaangaur/"},
};
#define NBOOK (int)(sizeof bookmarks / sizeof *bookmarks)
static rect book_r[NBOOK], search_r;

static void draw_start(void) {
    cur_url[0] = 0;
    fill(0, 0, W, H, WHITE);
    draw_top();
    icon_globe(W / 2, 330, 200);
    text_center(&f_serif_bold, 90, W / 2, 450, "The open web", BLACK);
    text_center(&f_serif_italic, 46, W / 2, 560, "in pages you can turn", 0x50);
    search_r = (rect){MARGIN, 660, W - 2 * MARGIN, 110};
    rrect_line(search_r.x + 1.5f, search_r.y + 1.5f, search_r.w - 3, search_r.h - 3, 55, 3, BLACK);
    ring(search_r.x + 60, search_r.y + 50, 18, 4, BLACK);
    line(search_r.x + 73, search_r.y + 63, search_r.x + 88, search_r.y + 78, 5, BLACK);
    text(&f_ui, 38, search_r.x + 110, search_r.y + 32, "Search the web", 0x70);
    text(&f_ui_bold, 30, MARGIN, 840, "BOOKMARKS", 0x60);
    for (int i = 0; i < NBOOK; i++) {
        book_r[i] = (rect){MARGIN, 900 + i * 86, W - 2 * MARGIN, 86};
        text(&f_serif_bold, 46, MARGIN, book_r[i].y + 14, bookmarks[i][0], BLACK);
        char host[128];
        url_host(bookmarks[i][1], host, sizeof host);
        text_right(&f_ui, 28, W - MARGIN, book_r[i].y + 30, host, 0x70);
        fill(MARGIN, book_r[i].y + 84, W - 2 * MARGIN, 1, 0xC0);
    }
    flip();
}

/* Loads url and shows its first page. Adds it to history unless replaying. */
static void go(const char *url, int record) {
    char u[2048];
    snprintf(u, sizeof u, "%s", url);
    unwrap_bing(u, sizeof u);
    toast("Loading...");
    if (fetch(u) || load_document()) {
        toast("Couldn't load that page");
        usleep(1200000);
        if (cur_url[0] && ntoks) draw_page(0); else draw_start();
        return;
    }
    if (record) {
        if (hist_n == 32) { memmove(history[0], history[1], sizeof history[0] * 31); hist_n--; }
        snprintf(history[hist_n++], sizeof history[0], "%s", cur_url);
    }
    paginate();
    cur_page = 0;
    draw_page(1);
}

static void ask_address(void) {
    char input[512] = "";
    kb_enter_label = "Go";
    if (!text_entry("Go to", "Search, or type a web address", input, sizeof input, 0)) {
        if (cur_url[0] && ntoks) draw_page(0); else draw_start();
        return;
    }
    char url[2048], q[1500];
    if (!input[0]) { draw_start(); return; }
    if (strstr(input, "://")) snprintf(url, sizeof url, "%s", input);
    else if (strchr(input, '.') && !strchr(input, ' ')) snprintf(url, sizeof url, "https://%s", input);
    else { url_encode(input, q, sizeof q); snprintf(url, sizeof url, "https://www.bing.com/search?q=%s", q); }
    go(url, 1);
}

int main(int argc, char **argv) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    doc_reset();
    if (argc > 1) go(argv[1], 1); else draw_start();
    for (;;) {
        event e = input_wait(-1);
        if (e.type == EV_POWER) return 0;
        int on_start = !cur_url[0];
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(addr_btn, e.x, e.y)) press_on(addr_btn);
            else if (hist_n > 1 && hit(hist_btn, e.x, e.y)) press_on(hist_btn);
            else if (on_start) {
                if (hit(search_r, e.x, e.y)) press_on(search_r);
                for (int i = 0; i < NBOOK; i++) if (hit(book_r[i], e.x, e.y)) press_on(book_r[i]);
            } else {
                if (cur_page > 0 && hit(prev_btn, e.x, e.y)) press_on(prev_btn);
                else if (cur_page < npages - 1 && hit(next_btn, e.x, e.y)) press_on(next_btn);
                else for (int i = 0; i < nboxes; i++) if (hit(boxes[i].r, e.x, e.y)) { press_on(boxes[i].r); break; }
            }
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) return 0;
        if (hit(addr_btn, e.x, e.y)) { press_forget(); ask_address(); continue; }
        if (hist_n > 1 && hit(hist_btn, e.x, e.y)) {
            press_forget();
            hist_n--;
            char u[2048];
            snprintf(u, sizeof u, "%s", history[hist_n - 1]);
            go(u, 0);
            continue;
        }
        if (on_start) {
            if (hit(search_r, e.x, e.y)) { press_forget(); ask_address(); continue; }
            int done = 0;
            for (int i = 0; i < NBOOK; i++) if (hit(book_r[i], e.x, e.y)) { press_forget(); go(bookmarks[i][1], 1); done = 1; break; }
            if (!done) press_off();
            continue;
        }
        int handled = 0;
        for (int i = 0; i < nboxes; i++)
            if (hit(boxes[i].r, e.x, e.y)) { press_forget(); go(links[boxes[i].link], 1); handled = 1; break; }
        if (handled) continue;
        int fwd = hit(next_btn, e.x, e.y) || (e.y > CONTENT_Y && e.y < CONTENT_B && e.x > W * 2 / 3);
        int back = hit(prev_btn, e.x, e.y) || (e.y > CONTENT_Y && e.y < CONTENT_B && e.x < W / 3);
        /* a swipe turns the page too */
        if (e.ux - e.x < -150) fwd = 1, back = 0;
        if (e.ux - e.x > 150) back = 1, fwd = 0;
        if (fwd && cur_page < npages - 1) { press_forget(); cur_page++; draw_page(0); }
        else if (back && cur_page > 0) { press_forget(); cur_page--; draw_page(0); }
        else press_off();
    }
}

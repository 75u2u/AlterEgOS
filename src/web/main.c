/*
 * alterego-web: the browser's terminal UI.
 *
 *   alterego-web [URL]              interactive, in the terminal it runs in
 *   alterego-web --dump [-w N] URL  print the page as text (and its links), for scripts and tests
 *
 * Keys: j/k or the arrows scroll, Space / PageDown and PageUp page, g/G
 * go to the top/bottom, a link number then Enter follows the link, o asks
 * for a URL, s for a search, b/f go back/forward in history, r reloads,
 * ? shows the help page, q quits. Esc cancels a prompt.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "../agent/utf8.h"
#include "web.h"

#define HOME_URL   "about:home"
#define SEARCH_URL "https://html.duckduckgo.com/html/?q="
#define HIST_MAX   100

static const char HOME_HTML[] =
    "<html><head><title>AlterEgOS ブラウザ</title></head><body>"
    "<h1>AlterEgOS のブラウザ</h1>"
    "<p>文字だけの小さなブラウザです。JavaScript や画像は扱いません。"
    "ページの中のリンクには <b>[番号]</b> が付きます。番号を打って Enter で、そのリンクへ進みます。</p>"
    "<h2>使い方</h2>"
    "<ul>"
    "<li><b>o</b> … URL を入力して開く</li>"
    "<li><b>s</b> … 言葉で検索する (DuckDuckGo)</li>"
    "<li><b>j / k</b>、矢印キー … 1 行ずつ動く。<b>Space</b> / PageDown、PageUp … 1 ページずつ</li>"
    "<li><b>g / G</b> … いちばん上 / いちばん下へ</li>"
    "<li><b>番号</b> + Enter … そのリンクへ進む</li>"
    "<li><b>b / f</b> … 戻る / 進む。<b>r</b> … 読み込み直す</li>"
    "<li><b>?</b> … この説明。<b>q</b> … 終了</li>"
    "</ul>"
    "<h2>行き先</h2>"
    "<ul>"
    "<li><a href=\"https://ja.wikipedia.org/\">Wikipedia 日本語版</a></li>"
    "<li><a href=\"https://html.duckduckgo.com/html/\">DuckDuckGo (検索)</a></li>"
    "<li><a href=\"https://www.kernel.org/\">kernel.org</a></li>"
    "<li><a href=\"https://github.com/75u2u/AlterEgOS\">AlterEgOS のリポジトリ</a></li>"
    "<li><a href=\"http://example.com/\">example.com (http)</a></li>"
    "</ul>"
    "<p>UTF-8 のページだけ正しく表示できます。Shift_JIS や EUC-JP のページは文字が化けます。</p>"
    "</body></html>";

struct ui {
    int rows, cols;
    struct termios saved;
    int raw;
    struct doc doc;
    struct layout lay;
    int top;                    /* first shown line */
    char url[4096];
    char *hist[HIST_MAX];
    int nhist, hpos;            /* hpos: index of the current page in hist */
    char msg[512];              /* the bottom line, until the next key */
    int mode;                   /* 0 page, 1 URL prompt, 2 search prompt */
    char prompt[2048];       /* URL or search words being typed */
    char digits[8];
    int insecure;
};

static struct ui g;
static volatile sig_atomic_t g_stop, g_resized;

static void on_signal(int sig) { if (sig == SIGWINCH) g_resized = 1; else g_stop = 1; }

/* ---- the terminal */

static void term_size(void)
{
    struct winsize ws;
    g.cols = 80;
    g.rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) { g.cols = ws.ws_col; g.rows = ws.ws_row; }
    if (g.cols < 20) g.cols = 20;
    if (g.rows < 4) g.rows = 4;
}

static void raw_on(void)
{
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g.saved) < 0) return;
    struct termios t = g.saved;
    cfmakeraw(&t);
    t.c_oflag |= OPOST | ONLCR;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g.raw = 1;
}

static void raw_off(void)
{
    if (g.raw) tcsetattr(STDIN_FILENO, TCSANOW, &g.saved);
    g.raw = 0;
}

static void write_all(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t k = write(STDOUT_FILENO, s, n);
        if (k < 0) { if (errno == EINTR) continue; return; }
        s += k;
        n -= (size_t)k;
    }
}

/* Cut `s` to at most `width` cells; returns the byte length kept. */
static size_t fit(const char *s, int width)
{
    size_t i = 0;
    int col = 0;
    while (s[i]) {
        uint32_t cp;
        size_t m = utf8_decode(s + i, &cp);
        if (!m) break;
        int w = (unsigned char)s[i] < 0x20 ? 0 : utf8_wide(cp) ? 2 : 1;
        if (col + w > width) break;
        col += w;
        i += m;
    }
    return i;
}

static int cells(const char *s)
{
    int col = 0;
    for (size_t i = 0; s[i]; ) {
        uint32_t cp;
        size_t m = utf8_decode(s + i, &cp);
        if (!m) break;
        col += (unsigned char)s[i] < 0x20 ? 0 : utf8_wide(cp) ? 2 : 1;
        i += m;
    }
    return col;
}

/* ---- pages */

static int page_lines(void) { return g.rows - 2; }

static void relayout(void)
{
    layout_free(&g.lay);
    layout_doc(&g.doc, g.cols - 1, &g.lay);
    if (g.top > g.lay.n - 1) g.top = g.lay.n - 1;
    if (g.top < 0) g.top = 0;
}

static void set_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.msg, sizeof g.msg, fmt, ap);
    va_end(ap);
}

static void show_error(const char *url, const char *what)
{
    char html[8192];
    snprintf(html, sizeof html, "<html><head><title>開けません</title></head><body><h1>開けません</h1><p>%s</p><p>%s</p>"
             "<p><a href=\"about:home\">はじめのページへ</a></p></body></html>", url, what);
    doc_free(&g.doc);
    html_to_text(html, strlen(html), url, "text/html", &g.doc);
}

/* Fetch `url` into g.doc. Returns 0 when a page (even an error page) is shown. */
static int load_into_doc(const char *url, struct doc *d, char *final_url, size_t fcap, char *note, size_t ncap, int *insecure)
{
    struct url u;
    note[0] = '\0';
    *insecure = 0;
    snprintf(final_url, fcap, "%s", url);
    if (url_parse(url, &u) < 0) {
        char fixed[4096];
        if (strchr(url, ' ') || !strchr(url, '.')) {                      /* words: search for them */
            char q[3000];
            url_encode(url, q, sizeof q);
            snprintf(fixed, sizeof fixed, SEARCH_URL "%s", q);
        } else snprintf(fixed, sizeof fixed, "http://%s", url);          /* a bare host */
        return load_into_doc(fixed, d, final_url, fcap, note, ncap, insecure);
    }
    if (strcmp(u.scheme, "about") == 0) {
        const char *html = HOME_HTML;
        if (strcmp(u.path, "blank") == 0) html = "<html><head><title>空のページ</title></head><body></body></html>";
        return html_to_text(html, strlen(html), url, "text/html", d);
    }
    if (strcmp(u.scheme, "file") == 0) {
        FILE *f = fopen(u.path, "rb");
        if (!f) { snprintf(note, ncap, "%.200s: %s", u.path, strerror(errno)); return -1; }
        char *buf = NULL;
        size_t len = 0, cap = 0;
        for (;;) {
            if (len + 65536 > cap) { cap = cap ? cap * 2 : 131072; char *nb = realloc(buf, cap); if (!nb) { free(buf); fclose(f); snprintf(note, ncap, "メモリ不足"); return -1; } buf = nb; }
            size_t k = fread(buf + len, 1, cap - len, f);
            if (k == 0) break;
            len += k;
            if (len > NET_MAX_BODY) break;
        }
        fclose(f);
        const char *ext = strrchr(u.path, '.');
        const char *ctype = ext && (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) ? "text/html" : "text/plain";
        int rc = html_to_text(buf ? buf : "", len, url, ctype, d);
        free(buf);
        return rc;
    }
    struct resp r;
    if (net_fetch(url, &r) < 0) { snprintf(note, ncap, "%s", r.err); resp_free(&r); return -1; }
    snprintf(final_url, fcap, "%s", r.url);
    *insecure = r.insecure;
    int rc = html_to_text(r.body ? r.body : "", r.len, r.url, r.ctype, d);
    if (rc == 0) {
        if (r.status >= 400) snprintf(note, ncap, "サーバーの応答: %d", r.status);
        const char *cs = strstr(r.ctype, "charset=");
        char charset[32] = "";
        if (cs) snprintf(charset, sizeof charset, "%.*s", (int)strcspn(cs + 8, " ;\"'"), cs + 8);
        else snprintf(charset, sizeof charset, "%s", d->charset);
        if (charset[0] && strcasecmp(charset, "utf-8") != 0 && strcasecmp(charset, "utf8") != 0 && strcasecmp(charset, "us-ascii") != 0)
            snprintf(note, ncap, "このページは UTF-8 ではありません (%s): 文字が化けます", charset);
        if (strncmp(r.ctype, "text/", 5) != 0 && !strstr(r.ctype, "html") && !strstr(r.ctype, "xml") && !strstr(r.ctype, "json") && r.ctype[0])
            snprintf(note, ncap, "文字のページではありません (%s)", r.ctype);
    }
    resp_free(&r);
    return rc;
}

static void push_history(const char *url)
{
    for (int i = g.hpos + 1; i < g.nhist; i++) free(g.hist[i]);
    g.nhist = g.hpos + 1;
    if (g.nhist == HIST_MAX) { free(g.hist[0]); memmove(g.hist, g.hist + 1, (size_t)(HIST_MAX - 1) * sizeof *g.hist); g.nhist--; }
    g.hist[g.nhist] = strdup(url);
    g.hpos = g.nhist++;
}

static void render(void);

static void go(const char *url_, int push)
{
    struct doc d;
    char url[4096], final_url[4096], note[512];
    int insecure;
    snprintf(url, sizeof url, "%s", url_);       /* url_ may live in the document or in g.url, which change below */
    set_msg("読み込み中: %s", url);
    render();
    if (load_into_doc(url, &d, final_url, sizeof final_url, note, sizeof note, &insecure) < 0) {
        show_error(url, note);
        snprintf(g.url, sizeof g.url, "%s", url);
        set_msg("%s", note);
    } else {
        doc_free(&g.doc);
        g.doc = d;
        snprintf(g.url, sizeof g.url, "%s", final_url);
        if (note[0]) set_msg("%s", note);
        else if (insecure) set_msg("証明書を確認できませんでした (CA 証明書がありません)");
        else set_msg("%d 個のリンク", g.doc.nlinks);
    }
    g.insecure = insecure;
    g.top = 0;
    g.digits[0] = '\0';
    if (push) push_history(g.url);
    else if (g.nhist > 0) { free(g.hist[g.hpos]); g.hist[g.hpos] = strdup(g.url); }
    relayout();
}

static void follow(int n)
{
    if (n < 1 || n > g.doc.nlinks) { set_msg("リンク [%d] はありません (1〜%d)", n, g.doc.nlinks); return; }
    go(g.doc.links[n - 1], 1);
}

/* ---- drawing */

static void render(void)
{
    static char *buf;
    static size_t cap;
    size_t need = (size_t)g.rows * ((size_t)g.cols * 4 + 64) + 8192;
    if (need > cap) { char *nb = realloc(buf, need); if (!nb) return; buf = nb; cap = need; }
    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o, "\x1b[H");
    int n = page_lines();
    for (int y = 0; y < n; y++) {
        o += (size_t)snprintf(buf + o, cap - o, "\x1b[K");
        int i = g.top + y;
        if (i < g.lay.n) o += layout_line(&g.doc, &g.lay, i, 1, buf + o, cap - o - 64);
        else if (g.lay.n > 0 && i == g.lay.n) o += (size_t)snprintf(buf + o, cap - o, "\x1b[90m(おわり)\x1b[0m");
        o += (size_t)snprintf(buf + o, cap - o, "\r\n");
    }
    /* status line: title, URL, position */
    char status[4600], pos[32], left[4400];
    int pct = g.lay.n <= n ? 100 : (int)(100L * (g.top + n) / g.lay.n);
    if (pct > 100) pct = 100;
    snprintf(pos, sizeof pos, " %d%% ", pct);
    if (g.doc.title[0]) snprintf(left, sizeof left, " %s%s - %s", g.insecure ? "(!) " : "", g.doc.title, g.url);
    else snprintf(left, sizeof left, " %s%s", g.insecure ? "(!) " : "", g.url);
    int room = g.cols - cells(pos) - 1;
    if (room < 4) room = 4;
    size_t keep = fit(left, room);
    snprintf(status, sizeof status, "%.*s", (int)keep, left);
    int pad = room - cells(status);
    o += (size_t)snprintf(buf + o, cap - o, "\x1b[K\x1b[7m%s%*s%s\x1b[0m\r\n", status, pad > 0 ? pad : 0, "", pos);
    /* bottom line: prompt, pending link number, or the message / key help */
    char bottom[2200];
    if (g.mode == 1) snprintf(bottom, sizeof bottom, "URL: %s", g.prompt);
    else if (g.mode == 2) snprintf(bottom, sizeof bottom, "検索: %s", g.prompt);
    else if (g.digits[0]) snprintf(bottom, sizeof bottom, "リンク番号: %s  (Enter で進む、Esc で取り消し)", g.digits);
    else if (g.msg[0]) snprintf(bottom, sizeof bottom, "%s", g.msg);
    else snprintf(bottom, sizeof bottom, "j k:移動 Space:次 番号+Enter:リンク o:URL s:検索 b:戻る r:再読込 ?:説明 q:終了");
    /* keep the end of a long prompt in view */
    const char *show = bottom;
    while (cells(show) > g.cols - 1 && *show) {
        uint32_t cp;
        size_t m = utf8_decode(show, &cp);
        if (!m) break;
        show += m;
    }
    o += (size_t)snprintf(buf + o, cap - o, "\x1b[K%s", show);
    write_all(buf, o);
}

/* ---- input */

static int read_key(char *seq, size_t cap)
{
    /* one key: a byte, a UTF-8 sequence, or ESC [ ... */
    unsigned char c;
    ssize_t k = read(STDIN_FILENO, &c, 1);
    if (k <= 0) return k < 0 && errno == EINTR ? 0 : -1;
    size_t n = 0;
    seq[n++] = (char)c;
    if (c == 0x1B) {
        struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
        while (n + 1 < cap && poll(&p, 1, 50) > 0) {
            if (read(STDIN_FILENO, &c, 1) <= 0) break;
            seq[n++] = (char)c;
            if (n == 2 && c != '[' && c != 'O') break;
            if (n > 2 && c >= 0x40 && c <= 0x7E) break;
        }
    } else if (c >= 0xC0) {
        int need = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
        while (need-- > 0 && n + 1 < cap && read(STDIN_FILENO, &c, 1) == 1) seq[n++] = (char)c;
    }
    seq[n] = '\0';
    return (int)n;
}

static void prompt_backspace(void)
{
    size_t n = strlen(g.prompt);
    if (!n) return;
    n--;
    while (n > 0 && ((unsigned char)g.prompt[n] & 0xC0) == 0x80) n--;
    g.prompt[n] = '\0';
}

static void key_page(const char *k)
{
    int n = page_lines(), max = g.lay.n - n;
    if (max < 0) max = 0;
    if (!strcmp(k, "\r") || !strcmp(k, "\n")) { if (g.digits[0]) { follow(atoi(g.digits)); g.digits[0] = '\0'; } return; }
    if (!strcmp(k, "\x1b[A") || !strcmp(k, "k")) g.top--;
    else if (!strcmp(k, "\x1b[B") || !strcmp(k, "j")) g.top++;
    else if (!strcmp(k, "\x1b[6~") || !strcmp(k, " ") || !strcmp(k, "\x1b[C")) g.top += n - 1;
    else if (!strcmp(k, "\x1b[5~") || !strcmp(k, "\x1b[D") || !strcmp(k, "u")) g.top -= n - 1;
    else if (!strcmp(k, "\x1b[H") || !strcmp(k, "g") || !strcmp(k, "\x1b[1~")) g.top = 0;
    else if (!strcmp(k, "\x1b[F") || !strcmp(k, "G") || !strcmp(k, "\x1b[4~")) g.top = max;
    else if (!strcmp(k, "o") || !strcmp(k, "O")) { g.mode = 1; snprintf(g.prompt, sizeof g.prompt, "%.2000s", k[0] == 'O' ? g.url : ""); }
    else if (!strcmp(k, "s") || !strcmp(k, "/")) { g.mode = 2; g.prompt[0] = '\0'; }
    else if (!strcmp(k, "b") || !strcmp(k, "\x7f") || !strcmp(k, "\b")) {
        if (g.digits[0]) { g.digits[strlen(g.digits) - 1] = '\0'; return; }
        if (g.hpos > 0) { g.hpos--; go(g.hist[g.hpos], 0); } else set_msg("これより前のページはありません");
    }
    else if (!strcmp(k, "f")) { if (g.hpos + 1 < g.nhist) { g.hpos++; go(g.hist[g.hpos], 0); } else set_msg("これより先のページはありません"); }
    else if (!strcmp(k, "r")) go(g.url, 0);
    else if (!strcmp(k, "?") || !strcmp(k, "h")) go("about:home", 1);
    else if (!strcmp(k, "q") || !strcmp(k, "\x03")) g_stop = 1;
    else if (!strcmp(k, "\x1b")) { g.digits[0] = '\0'; g.msg[0] = '\0'; }
    else if (k[0] >= '0' && k[0] <= '9' && !k[1]) {
        size_t n2 = strlen(g.digits);
        if (n2 + 1 < sizeof g.digits) { g.digits[n2] = k[0]; g.digits[n2 + 1] = '\0'; }
        g.msg[0] = '\0';
    }
    if (g.top > max) g.top = max;
    if (g.top < 0) g.top = 0;
}

static void key_prompt(const char *k)
{
    if (!strcmp(k, "\x1b") || !strcmp(k, "\x03")) { g.mode = 0; return; }
    if (!strcmp(k, "\x7f") || !strcmp(k, "\b")) { prompt_backspace(); return; }
    if (!strcmp(k, "\x15")) { g.prompt[0] = '\0'; return; }
    if (!strcmp(k, "\r") || !strcmp(k, "\n")) {
        int mode = g.mode;
        g.mode = 0;
        char *p = g.prompt;
        while (*p == ' ') p++;
        size_t n = strlen(p);
        while (n > 0 && p[n - 1] == ' ') p[--n] = '\0';
        if (!n) return;
        if (mode == 2) {
            char q[4096], url[4200];
            url_encode(p, q, sizeof q);
            snprintf(url, sizeof url, SEARCH_URL "%s", q);
            go(url, 1);
        } else go(p, 1);
        return;
    }
    if ((unsigned char)k[0] < 0x20) return;
    size_t n = strlen(g.prompt), m = strlen(k);
    if (n + m < sizeof g.prompt) memcpy(g.prompt + n, k, m + 1);
}

/* ---- main */

static int dump(const char *url, int width)
{
    struct doc d;
    char final_url[4096], note[512];
    int insecure;
    if (load_into_doc(url, &d, final_url, sizeof final_url, note, sizeof note, &insecure) < 0) {
        fprintf(stderr, "alterego-web: %s: %s\n", url, note);
        return 1;
    }
    struct layout l;
    layout_doc(&d, width, &l);
    if (d.title[0]) printf("# %s\n\n", d.title);
    char line[8192];
    for (int i = 0; i < l.n; i++) {
        layout_line(&d, &l, i, 0, line, sizeof line);
        puts(line);
    }
    if (d.nlinks) {
        puts("");
        puts("リンク:");
        for (int i = 0; i < d.nlinks; i++) printf("[%d] %s\n", i + 1, d.links[i]);
    }
    if (note[0]) fprintf(stderr, "alterego-web: %s\n", note);
    layout_free(&l);
    doc_free(&d);
    return 0;
}

static void cleanup(void)
{
    if (g.raw) { write_all("\x1b[0m\x1b[2J\x1b[H", 10); raw_off(); }
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    int do_dump = 0, width = 80;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump")) do_dump = 1;
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: alterego-web [URL]\n       alterego-web --dump [-w columns] URL\n%s\n", net_has_tls() ? "https: yes (mbedTLS)" : "https: no (built without TLS)");
            return 0;
        } else url = argv[i];
    }
    if (do_dump) {
        if (!url) { fprintf(stderr, "alterego-web --dump: URL がありません\n"); return 2; }
        return dump(url, width > 8 ? width : 8);
    }
    if (!url) url = getenv("ALTEREGO_HOME");
    if (!url) url = HOME_URL;

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    atexit(cleanup);
    raw_on();
    term_size();
    write_all("\x1b[2J", 4);
    go(url, 1);
    render();
    while (!g_stop) {
        char k[16];
        int n = read_key(k, sizeof k);
        if (n < 0) break;
        if (g_resized) { g_resized = 0; term_size(); relayout(); write_all("\x1b[2J", 4); }
        if (n == 0) { render(); continue; }
        if (g.mode) key_prompt(k);
        else { g.msg[0] = '\0'; key_page(k); }
        if (g_stop) break;
        render();
    }
    for (int i = 0; i < g.nhist; i++) free(g.hist[i]);
    layout_free(&g.lay);
    doc_free(&g.doc);
    return 0;
}

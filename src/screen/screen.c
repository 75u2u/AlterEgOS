/*
 * AlterEgOS screen: the desktop.
 *
 * Draws straight onto the framebuffer and reads keyboards and mice from
 * evdev; no X, no Wayland, no toolkit. Three windows, each a small
 * terminal on its own pty: AlterEgo (the agent, chat), a shell (busybox
 * sh, the escape hatch) and the browser (alterego-web, hidden until its
 * button is pressed). Windows are raised, focused, moved and closed with
 * the mouse; the bar at the top has buttons to reopen them, to switch the
 * Japanese input (romaji to kana, Space for kanji) on and off, and to
 * reboot or power off (PID 1 does the actual work when signalled).
 *
 * PID 1 starts this when the console is a screen (tty0). Without a
 * framebuffer it exits 127, and init falls back to the agent on tty1.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "canvas.h"
#include "dict.h"
#include "fb.h"
#include "font.h"
#include "ime.h"
#include "input.h"
#include "pty.h"
#include "term.h"

#ifndef ALTEREGOS_VERSION
#define ALTEREGOS_VERSION "0.0.1-dev"
#endif

#define BAR_H     24
#define TITLE_H   20
#define PAD       4
#define WIN_MAX   3

#define C_DESK    0x1E2430
#define C_BAR     0x2B3340
#define C_BARTEXT 0xDDE3EA
#define C_BTN     0x3C4756
#define C_BTN_ON  0x3B6EA5
#define C_WINBG   0x0F1317
#define C_TEXT    0xE6E6E6
#define C_TITLE   0x3B6EA5
#define C_TITLE_U 0x3A4250
#define C_BORDER  0x8A94A0
#define C_PREEDIT 0xFFD866
#define C_CONV    0x7FD8FF
#define C_POPUP   0x2B3340
#define C_CURSOR  0xE6E6E6
#define C_BOLD    0xFFFFFF

/* The 16 ANSI colors, as cell.fg 1..16. */
static const uint32_t g_ansi[16] = {
    0x3A4250, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xD0D0D0,
    0x7F8894, 0xFF8A94, 0xB5E890, 0xFFD866, 0x7FC8FF, 0xE0A0FF, 0x7FE0E8, 0xFFFFFF };

enum { W_CHAT = 0, W_SHELL = 1, W_WEB = 2 };

struct win {
    const char *title;
    struct term term;
    int fd;
    pid_t pid;
    int x, y, cols, rows;
    int visible;
    time_t respawn_at;
    char *const *argv;
};

struct button { int x, y, w, h; int action; };
enum { A_NONE, A_CHAT, A_SHELL, A_WEB, A_KANA, A_REBOOT, A_POWEROFF };

static struct canvas g_cv;
static struct fb g_fb;
static struct font g_font;
static struct input g_in;
static struct dict g_dict;
static struct ime g_ime;
static struct win g_win[WIN_MAX];
static int g_order[WIN_MAX] = { W_SHELL, W_WEB, W_CHAT };     /* bottom to top */
static int g_focus = W_CHAT;
static int g_kana_mode = 0;
static int g_mx, g_my;                                  /* mouse pointer */
static int g_drag_win = -1, g_drag_dx, g_drag_dy;
static struct button g_btn[8];
static int g_nbtn;
static int g_dirty = 1;
static int g_vt_fd = -1;
static volatile sig_atomic_t g_stop = 0;
static const char *g_snapshot;                          /* headless: write each frame here as PPM */
static const char *g_script;                            /* headless: keys to type, then exit */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", g_cv.w, g_cv.h);
    for (size_t i = 0; i < (size_t)g_cv.w * g_cv.h; i++) {
        uint32_t p = g_cv.px[i];
        fputc((p >> 16) & 0xFF, f); fputc((p >> 8) & 0xFF, f); fputc(p & 0xFF, f);
    }
    fclose(f);
}

static void on_signal(int sig) { g_stop = sig; }

static void kmsg(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { dprintf(fd, "<6>alterego-screen: %s\n", line); close(fd); }
    else fprintf(stderr, "alterego-screen: %s\n", line);
}

/* ---- windows */

static int win_w(const struct win *w) { return w->cols * FONT_W + 2 * PAD + 2; }
static int win_h(const struct win *w) { return TITLE_H + w->rows * FONT_H + 2 * PAD + 2; }

static char **child_env(void)
{
    static char *env[12];
    static char persona[256], state[256], home[4200], ca[300];
    int n = 0;
    env[n++] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
    env[n++] = "HOME=/root";
    env[n++] = "TERM=linux";
    env[n++] = "LANG=ja_JP.UTF-8";
    const char *p = getenv("ALTEREGO_PERSONA");
    if (p) { snprintf(persona, sizeof persona, "ALTEREGO_PERSONA=%s", p); env[n++] = persona; }
    p = getenv("ALTEREGO_STATE");
    if (p) { snprintf(state, sizeof state, "ALTEREGO_STATE=%s", p); env[n++] = state; }
    p = getenv("ALTEREGO_HOME");                                      /* the browser's first page */
    if (p) { snprintf(home, sizeof home, "ALTEREGO_HOME=%.4096s", p); env[n++] = home; }
    p = getenv("ALTEREGO_CA");                                        /* its CA bundle (tests) */
    if (p) { snprintf(ca, sizeof ca, "ALTEREGO_CA=%.256s", p); env[n++] = ca; }
    env[n] = NULL;
    return env;
}

static void win_spawn(struct win *w)
{
    w->fd = pty_spawn(w->argv, child_env(), w->cols, w->rows, &w->pid);
    w->respawn_at = 0;
    if (w->fd < 0) { kmsg("%s: cannot start %s: %s", w->title, w->argv[0], strerror(errno)); w->respawn_at = time(NULL) + 5; }
    g_dirty = 1;
}

static void win_init(struct win *w, const char *title, char *const argv[], int cols, int rows, int x, int y)
{
    memset(w, 0, sizeof *w);
    w->title = title;
    w->argv = argv;
    w->cols = cols;
    w->rows = rows;
    w->x = x;
    w->y = y;
    w->fd = -1;
    w->visible = 1;
    term_init(&w->term, cols, rows);
}

static void raise_win(int i)
{
    int pos = -1;
    for (int k = 0; k < WIN_MAX; k++) if (g_order[k] == i) pos = k;
    for (int k = pos; k < WIN_MAX - 1; k++) g_order[k] = g_order[k + 1];
    g_order[WIN_MAX - 1] = i;
    g_focus = i;
    g_win[i].visible = 1;
    g_dirty = 1;
}

static void win_write(struct win *w, const char *s, size_t n)
{
    if (w->fd < 0) return;
    while (n > 0) {
        ssize_t k = write(w->fd, s, n);
        if (k < 0) { if (errno == EINTR) continue; break; }
        s += k;
        n -= (size_t)k;
    }
}

/* ---- drawing */

static void button(int x, int y, const char *label, int on, int action)
{
    int w = font_text_width(&g_font, label) + 12;
    canvas_fill(&g_cv, x, y, w, BAR_H - 6, on ? C_BTN_ON : C_BTN);
    font_text(&g_font, &g_cv, x + 6, y + 1, label, C_BARTEXT, on ? C_BTN_ON : C_BTN);
    if (g_nbtn < (int)(sizeof g_btn / sizeof g_btn[0])) g_btn[g_nbtn++] = (struct button){ x, y, w, BAR_H - 6, action };
}

static void draw_bar(void)
{
    canvas_fill(&g_cv, 0, 0, g_cv.w, BAR_H, C_BAR);
    canvas_hline(&g_cv, 0, BAR_H - 1, g_cv.w, C_BORDER);
    g_nbtn = 0;
    int x = 8;
    x = font_text(&g_font, &g_cv, x, 4, "AlterEgOS " ALTEREGOS_VERSION, C_BARTEXT, C_BAR) + 16;
    button(x, 3, "AlterEgo", g_focus == W_CHAT && g_win[W_CHAT].visible, A_CHAT);
    x += font_text_width(&g_font, "AlterEgo") + 12 + 6;
    button(x, 3, "シェル", g_focus == W_SHELL && g_win[W_SHELL].visible, A_SHELL);
    x += font_text_width(&g_font, "シェル") + 12 + 6;
    button(x, 3, "ブラウザ", g_focus == W_WEB && g_win[W_WEB].visible, A_WEB);
    x += font_text_width(&g_font, "シェル") + 12 + 16;
    button(x, 3, g_kana_mode ? "あ かな入力" : "A 英数入力", g_kana_mode, A_KANA);

    char clock[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(clock, sizeof clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    int right = g_cv.w - 8;
    int w = font_text_width(&g_font, "電源オフ") + 12;
    button(right - w, 3, "電源オフ", 0, A_POWEROFF);
    right -= w + 6;
    w = font_text_width(&g_font, "再起動") + 12;
    button(right - w, 3, "再起動", 0, A_REBOOT);
    right -= w + 16;
    font_text(&g_font, &g_cv, right - font_text_width(&g_font, clock), 4, clock, C_BARTEXT, C_BAR);
}

static void draw_win(int i)
{
    struct win *w = &g_win[i];
    if (!w->visible) return;
    int focused = g_focus == i;
    int W = win_w(w), H = win_h(w);
    canvas_fill(&g_cv, w->x, w->y, W, H, C_WINBG);
    canvas_fill(&g_cv, w->x, w->y, W, TITLE_H, focused ? C_TITLE : C_TITLE_U);
    canvas_frame(&g_cv, w->x, w->y, W, H, C_BORDER);
    font_text(&g_font, &g_cv, w->x + 8, w->y + 2, w->title, C_BARTEXT, focused ? C_TITLE : C_TITLE_U);
    /* close box */
    canvas_fill(&g_cv, w->x + W - TITLE_H + 2, w->y + 3, TITLE_H - 6, TITLE_H - 6, C_BTN);
    font_draw(&g_font, &g_cv, w->x + W - TITLE_H + 5, w->y + 2, 'x', C_BARTEXT, C_BTN);
    if (w->fd < 0) {
        font_text(&g_font, &g_cv, w->x + 8, w->y + 2 + font_text_width(&g_font, w->title) + 16, "(停止中)", C_BARTEXT, focused ? C_TITLE : C_TITLE_U);
    }
    int ox = w->x + 1 + PAD, oy = w->y + TITLE_H + PAD;
    for (int y = 0; y < w->rows; y++) {
        for (int x = 0; x < w->cols; x++) {
            const struct cell *c = w->term.cells + (size_t)y * w->cols + x;
            if (c->kind == CELL_TAIL) continue;
            int cursor = focused && x == w->term.cx && y == w->term.cy;
            uint32_t cp = c->cp ? c->cp : ' ';
            uint32_t fg = c->fg ? g_ansi[(c->fg - 1) & 15] : (c->attr & ATTR_BOLD) ? C_BOLD : C_TEXT, bg = C_WINBG;
            if (c->attr & ATTR_REVERSE) { uint32_t tmp = fg; fg = bg; bg = tmp; }
            if (cursor) { fg = C_WINBG; bg = C_CURSOR; }
            int px = ox + x * FONT_W, py = oy + y * FONT_H;
            font_draw(&g_font, &g_cv, px, py, cp, fg, bg);
            if (c->attr & ATTR_UNDERLINE) canvas_hline(&g_cv, px, py + FONT_H - 1, (c->kind == CELL_WIDE ? 2 : 1) * FONT_W, fg);
        }
    }
    if (focused && g_kana_mode && ime_active(&g_ime)) {  /* the preedit, at the cursor */
        char shown[IME_PRE + 64], popup[IME_CANDS * (DICT_CAND + 8)];
        size_t conv;
        int cur;
        ime_shown(&g_ime, shown, sizeof shown, &conv);
        int px = ox + w->term.cx * FONT_W, py = oy + w->term.cy * FONT_H;
        int ex = px;
        if (conv) {                                      /* the candidate, highlighted */
            char head[IME_PRE];
            snprintf(head, sizeof head, "%.*s", (int)conv, shown);
            ex = font_text(&g_font, &g_cv, px, py, head, C_WINBG, C_CONV);
        }
        int ex2 = font_text(&g_font, &g_cv, ex, py, shown + conv, C_PREEDIT, C_WINBG);
        canvas_hline(&g_cv, px, py + FONT_H - 1, ex2 - px, C_PREEDIT);
        ime_popup(&g_ime, popup, sizeof popup, &cur);
        if (popup[0]) {                                  /* the candidate list, under the cursor */
            int pw = font_text_width(&g_font, popup) + 12;
            int ppx = px, ppy = py + FONT_H + 2;
            if (ppx + pw > g_cv.w) ppx = g_cv.w - pw;
            if (ppx < 0) ppx = 0;
            if (ppy + FONT_H + 4 > g_cv.h) ppy = py - FONT_H - 6;
            canvas_fill(&g_cv, ppx, ppy, pw, FONT_H + 4, C_POPUP);
            canvas_frame(&g_cv, ppx, ppy, pw, FONT_H + 4, C_BORDER);
            font_text(&g_font, &g_cv, ppx + 6, ppy + 2, popup, C_BARTEXT, C_POPUP);
        }
    }
}

static void draw_pointer(void)
{
    static const char *arrow[] = {
        "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ", "X.....X    ",
        "X......X   ", "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ", "X.X X..X   ", "XX  X..X   ",
        "     X..X  ", "      XX   " };
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 11; x++) {
            char ch = arrow[y][x];
            if (ch == ' ') continue;
            int px = g_mx + x, py = g_my + y;
            if (px < 0 || py < 0 || px >= g_cv.w || py >= g_cv.h) continue;
            g_cv.px[(size_t)py * g_cv.w + px] = ch == 'X' ? 0x000000 : 0xFFFFFF;
        }
}

static void render(void)
{
    canvas_fill(&g_cv, 0, 0, g_cv.w, g_cv.h, C_DESK);
    for (int k = 0; k < WIN_MAX; k++) draw_win(g_order[k]);
    draw_bar();
    draw_pointer();
    if (g_fb.mem) fb_blit(&g_fb, &g_cv);
    else if (g_snapshot) write_ppm(g_snapshot);
    g_dirty = 0;
}

/* ---- input */

static int win_at(int x, int y)
{
    for (int k = WIN_MAX - 1; k >= 0; k--) {
        const struct win *w = &g_win[g_order[k]];
        if (!w->visible) continue;
        if (x >= w->x && x < w->x + win_w(w) && y >= w->y && y < w->y + win_h(w)) return g_order[k];
    }
    return -1;
}

static void action(int a)
{
    switch (a) {
    case A_CHAT: raise_win(W_CHAT); break;
    case A_SHELL: raise_win(W_SHELL); break;
    case A_WEB: raise_win(W_WEB); break;
    case A_KANA: {
        char out[IME_PRE + DICT_CAND];
        size_t n = ime_flush(&g_ime, out, sizeof out);
        if (n) win_write(&g_win[g_focus], out, n);
        g_kana_mode = !g_kana_mode;
        g_dirty = 1;
        break;
    }
    case A_REBOOT: if (getppid() == 1) kill(1, SIGINT); break;        /* init reboots */
    case A_POWEROFF: if (getppid() == 1) kill(1, SIGTERM); break;     /* init powers off */
    default: break;
    }
}

static void on_click(int down)
{
    if (!down) { g_drag_win = -1; return; }
    if (g_my < BAR_H) {
        for (int i = 0; i < g_nbtn; i++) {
            const struct button *b = &g_btn[i];
            if (g_mx >= b->x && g_mx < b->x + b->w && g_my >= b->y && g_my < b->y + b->h) { action(b->action); return; }
        }
        return;
    }
    int i = win_at(g_mx, g_my);
    if (i < 0) return;
    struct win *w = &g_win[i];
    if (g_my < w->y + TITLE_H) {
        if (g_mx >= w->x + win_w(w) - TITLE_H) { w->visible = 0; g_dirty = 1; 
            for (int k = WIN_MAX - 1; k >= 0; k--) if (g_win[g_order[k]].visible) { g_focus = g_order[k]; break; }
            return; }
        g_drag_win = i;
        g_drag_dx = g_mx - w->x;
        g_drag_dy = g_my - w->y;
    }
    raise_win(i);
}

static void on_key(const struct input_ev *e)
{
    struct win *w = &g_win[g_focus];
    if (e->type == IN_KEY) {
        if (e->key == KEY_ZENKAKUHANKAKU || e->key == KEY_KATAKANAHIRAGANA || (e->ctrl && e->key == KEY_SPACE)) action(A_KANA);
        else if (e->alt && e->key == KEY_TAB) raise_win((g_focus + 1) % WIN_MAX);      /* chat, shell, browser, chat ... */
        return;
    }
    if (!w->visible) { w->visible = 1; g_dirty = 1; }
    const char *t = e->text;
    if (g_kana_mode && !e->ctrl) {
        char out[IME_PRE + DICT_CAND + 16];
        size_t n = ime_key(&g_ime, t, out, sizeof out);
        if (n) win_write(w, out, n);
        g_dirty = 1;
        return;
    }
    win_write(w, t, strlen(t));
}

static void on_input(const struct input_ev *e)
{
    switch (e->type) {
    case IN_KEY: case IN_TEXT: on_key(e); break;
    case IN_MOUSE_MOVE:
        g_mx += e->dx; g_my += e->dy;
        goto moved;
    case IN_MOUSE_ABS:
        g_mx = e->ax_max > 0 ? (int)((long)e->ax * (g_cv.w - 1) / e->ax_max) : g_mx;
        g_my = e->ay_max > 0 ? (int)((long)e->ay * (g_cv.h - 1) / e->ay_max) : g_my;
    moved:
        if (g_mx < 0) g_mx = 0;
        if (g_my < 0) g_my = 0;
        if (g_mx >= g_cv.w) g_mx = g_cv.w - 1;
        if (g_my >= g_cv.h) g_my = g_cv.h - 1;
        if (g_drag_win >= 0) {
            struct win *w = &g_win[g_drag_win];
            w->x = g_mx - g_drag_dx;
            w->y = g_my - g_drag_dy;
            if (w->y < BAR_H) w->y = BAR_H;
            if (w->x < -win_w(w) + 40) w->x = -win_w(w) + 40;
            if (w->x > g_cv.w - 40) w->x = g_cv.w - 40;
            if (w->y > g_cv.h - TITLE_H) w->y = g_cv.h - TITLE_H;
        }
        g_dirty = 1;
        break;
    case IN_BUTTON: if (e->button == 0) on_click(e->down); break;
    default: break;
    }
}

/* ---- setup */

static void vt_graphics(int on)
{
    if (g_vt_fd < 0) return;
    ioctl(g_vt_fd, KDSETMODE, on ? KD_GRAPHICS : KD_TEXT);
}

static void cleanup(void)
{
    vt_graphics(0);
    input_close(&g_in);
}

static void reap(void)
{
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        for (int i = 0; i < WIN_MAX; i++) {
            struct win *w = &g_win[i];
            if (w->pid != pid) continue;
            close(w->fd);
            w->fd = -1;
            w->pid = 0;
            w->respawn_at = time(NULL) + 1;
            char note[96];
            snprintf(note, sizeof note, "\r\n[%s を終了しました。1 秒後にもう一度起動します]\r\n", w->argv[0]);
            term_feed(&w->term, note, strlen(note));
            g_dirty = 1;
        }
    }
}

/* Headless: every window's text, for the tests. */
static void dump_windows(const char *path)
{
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < WIN_MAX; i++) {
        fprintf(f, "== %s\n", g_win[i].title);
        char row[1024];
        for (int y = 0; y < g_win[i].rows; y++) { term_row(&g_win[i].term, y, row, sizeof row); fprintf(f, "%s\n", row); }
    }
    fclose(f);
}

int main(void)
{
    const char *fbdev = getenv("ALTEREGO_FB");
    if (!fbdev) fbdev = "/dev/fb0";
    const char *fontpath = getenv("ALTEREGO_FONT");
    if (!fontpath) fontpath = "/usr/share/alterego/font.hex";
    const char *dictpath = getenv("ALTEREGO_DICT");
    if (!dictpath) dictpath = "/usr/share/alterego/dict.txt";

    /* Headless (tests, screenshots): ALTEREGO_HEADLESS=WxH draws into memory,
     * ALTEREGO_SNAPSHOT=file.ppm keeps the last frame, ALTEREGO_SCRIPT types
     * keys into the focused window (\n is Enter; ALTEREGO_SCRIPT_WIN=shell|web
     * focuses that window first), ALTEREGO_DUMP=file gets every window's text
     * at the end, and the program then exits. */
    const char *headless = getenv("ALTEREGO_HEADLESS");
    g_snapshot = getenv("ALTEREGO_SNAPSHOT");
    g_script = getenv("ALTEREGO_SCRIPT");
    memset(&g_fb, 0, sizeof g_fb);
    g_fb.fd = -1;
    if (headless) {
        if (sscanf(headless, "%dx%d", &g_fb.w, &g_fb.h) != 2 || g_fb.w < 320 || g_fb.h < 200) { g_fb.w = 1024; g_fb.h = 768; }
        g_fb.bpp = 32;
        fbdev = "memory";
    } else {
        int tries;
        for (tries = 0; tries < 10 && fb_open(&g_fb, fbdev) < 0; tries++) usleep(500 * 1000);
        if (tries == 10) {
            kmsg("no framebuffer at %s (%s): no screen, leaving the console to the agent", fbdev, strerror(errno));
            return 127;
        }
    }
    if (font_load(&g_font, fontpath) < 0) {
        kmsg("cannot read the font %s: %s", fontpath, strerror(errno));
        return 1;
    }
    if (canvas_init(&g_cv, g_fb.w, g_fb.h) < 0) return 1;
    if (dict_load(&g_dict, dictpath) < 0) kmsg("no kanji dictionary at %s (%s): kana only", dictpath, strerror(errno));

    if (!headless) {
        if (isatty(STDIN_FILENO)) g_vt_fd = STDIN_FILENO;
        else g_vt_fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
        vt_graphics(1);
    }
    atexit(cleanup);
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    input_init(&g_in);
    input_scan(&g_in);
    int kbd = 0, ptr = 0;
    for (int i = 0; i < g_in.n; i++) { if (g_in.dev[i].kind == DEV_KEYBOARD) kbd++; else ptr++; }

    /* Window sizes that fit the screen (80x28 for the chat on 1024x768). */
    int ccols = (g_fb.w - 48) / FONT_W, crows = (g_fb.h - BAR_H - 140) / FONT_H;
    if (ccols > 80) ccols = 80;
    if (crows > 28) crows = 28;
    if (ccols < 20) ccols = 20;
    if (crows < 5) crows = 5;
    int scols = ccols > 72 ? 72 : ccols, srows = crows > 18 ? 18 : crows;

    static char *const chat_argv[] = { "/usr/bin/alterego", NULL };
    static char *const shell_argv[] = { "/bin/sh", NULL };
    static char *const web_argv[] = { "/usr/bin/alterego-web", NULL };
    static char agent_path[256], shell_path[256], web_path[256];
    char *const *ca = chat_argv, *const *sa_ = shell_argv, *const *wa = web_argv;
    static char *alt_chat[2], *alt_shell[2], *alt_web[2];
    const char *p;
    if ((p = getenv("ALTEREGO_AGENT"))) { snprintf(agent_path, sizeof agent_path, "%s", p); alt_chat[0] = agent_path; ca = alt_chat; }
    if ((p = getenv("ALTEREGO_SHELL"))) { snprintf(shell_path, sizeof shell_path, "%s", p); alt_shell[0] = shell_path; sa_ = alt_shell; }
    if ((p = getenv("ALTEREGO_WEB"))) { snprintf(web_path, sizeof web_path, "%s", p); alt_web[0] = web_path; wa = alt_web; }

    win_init(&g_win[W_CHAT], "AlterEgo", ca, ccols, crows, 16, BAR_H + 12);
    int sw = scols * FONT_W + 2 * PAD + 2, sh = TITLE_H + srows * FONT_H + 2 * PAD + 2;
    win_init(&g_win[W_SHELL], "シェル (busybox sh)", sa_, scols, srows, g_fb.w - sw - 16, g_fb.h - sh - 16);
    /* The browser: as wide as the screen allows (up to 100 columns), hidden
     * until the ブラウザ button is pressed; it starts on its help page. */
    int wcols = (g_fb.w - 48) / FONT_W, wrows = (g_fb.h - BAR_H - 60) / FONT_H;
    if (wcols > 100) wcols = 100;
    if (wrows > 40) wrows = 40;
    if (wcols < 20) wcols = 20;
    if (wrows < 5) wrows = 5;
    int ww = wcols * FONT_W + 2 * PAD + 2;
    win_init(&g_win[W_WEB], "ブラウザ (alterego-web)", wa, wcols, wrows, g_fb.w - ww - 16, BAR_H + 12);
    g_win[W_WEB].visible = 0;
    g_mx = g_fb.w / 2;
    g_my = g_fb.h / 2;
    ime_init(&g_ime, g_dict.idx ? &g_dict : NULL);
    for (int i = 0; i < WIN_MAX; i++) win_spawn(&g_win[i]);

    kmsg("up %dx%d %dbpp on %s, %d glyphs, %d dictionary entries, %d keyboards, %d pointers, agent on pty",
         g_fb.w, g_fb.h, g_fb.bpp, fbdev, g_font.n, g_dict.n, kbd, ptr);
    render();

    time_t last_scan = time(NULL), last_minute = time(NULL) / 60;
    long script_next = now_ms() + 1500;
    size_t script_pos = 0;
    if (g_script && getenv("ALTEREGO_SCRIPT_KANA")) g_kana_mode = 1;
    if (g_script && (p = getenv("ALTEREGO_SCRIPT_WIN"))) raise_win(strcmp(p, "web") == 0 ? W_WEB : strcmp(p, "shell") == 0 ? W_SHELL : W_CHAT);
    while (!g_stop) {
        if (g_script && now_ms() >= script_next) {
            if (g_script[script_pos]) {
                struct input_ev e = { .type = IN_TEXT };
                char c = g_script[script_pos++];
                e.text[0] = c == '\n' ? '\r' : c;
                on_key(&e);
                script_next = now_ms() + (c == '\n' ? 700 : 30);
            } else if (now_ms() >= script_next + 1500) {
                render();
                dump_windows(getenv("ALTEREGO_DUMP"));
                break;
            }
        }
        struct pollfd pfd[WIN_MAX + INPUT_MAX_DEV];
        int n = 0;
        int widx[WIN_MAX];
        for (int i = 0; i < WIN_MAX; i++) {
            widx[i] = -1;
            if (g_win[i].fd >= 0) { widx[i] = n; pfd[n].fd = g_win[i].fd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
        }
        int first_in = n;
        n += input_pollfds(&g_in, pfd + n, INPUT_MAX_DEV);
        int r = poll(pfd, (nfds_t)n, g_script ? 30 : 500);
        if (r < 0 && errno != EINTR) break;
        for (int i = 0; i < WIN_MAX; i++) {
            if (widx[i] < 0 || !(pfd[widx[i]].revents & (POLLIN | POLLHUP))) continue;
            char buf[4096];
            ssize_t k = read(g_win[i].fd, buf, sizeof buf);
            if (k > 0) { term_feed(&g_win[i].term, buf, (size_t)k); g_dirty = 1; }
        }
        for (int d = g_in.n - 1; d >= 0; d--) {
            if (first_in + d >= n || !(pfd[first_in + d].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            struct input_ev ev[64];
            int m = input_read(&g_in, d, ev, 64);
            for (int i = 0; i < m; i++) on_input(&ev[i]);
        }
        reap();
        time_t now = time(NULL);
        for (int i = 0; i < WIN_MAX; i++)
            if (g_win[i].fd < 0 && g_win[i].respawn_at && now >= g_win[i].respawn_at) win_spawn(&g_win[i]);
        if (now - last_scan >= 2) { last_scan = now; if (input_scan(&g_in) != kbd + ptr) { kbd = ptr = 0; for (int i = 0; i < g_in.n; i++) { if (g_in.dev[i].kind == DEV_KEYBOARD) kbd++; else ptr++; } } }
        if (now / 60 != last_minute) { last_minute = now / 60; g_dirty = 1; }
        if (g_dirty) render();
    }
    for (int i = 0; i < WIN_MAX; i++) if (g_win[i].pid > 0) kill(g_win[i].pid, SIGTERM);
    return 0;
}

/* Windows and pictures asked for in conversation. See apps.h. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "apps.h"
#include "shapes.h"
#include "store.h"
#include "utf8.h"

static int has(const char *s, const char *needle) { return strstr(s, needle) != NULL; }

static int has_word(const char *lo, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = strstr(lo, w); p; p = strstr(p + 1, w))
        if ((p == lo || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[n])) return 1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* windows */

enum { APP_NONE = -1, APP_CHAT, APP_SHELL, APP_WEB, APP_PAINT };
static const char *const app_id[] = { "chat", "shell", "web", "paint" };
static const char *const app_ja[] = { "チャット", "シェル", "ブラウザ", "ペイント" };
static const char *const app_en[] = { "the chat", "the shell", "the browser", "paint" };

static int app_in(const char *line, const char *lo)
{
    if (has(line, "ペイント") || has(line, "お絵かき") || has(line, "おえかき") || has_word(lo, "paint")) return APP_PAINT;
    if (has(line, "ブラウザ") || has_word(lo, "browser") || has_word(lo, "web")) return APP_WEB;
    if (has(line, "シェル") || has(line, "ターミナル") || has(line, "端末") || has_word(lo, "shell") || has_word(lo, "terminal")) return APP_SHELL;
    if (has(line, "チャット") || has_word(lo, "chat")) return APP_CHAT;
    return APP_NONE;
}

static int wants_open(const char *line, const char *lo)
{
    return has(line, "開いて") || has(line, "開け") || has(line, "ひらいて") || has(line, "起動") || has(line, "出して") || has(line, "だして") || has(line, "立ち上げ") ||
           has_word(lo, "open") || has_word(lo, "launch") || has_word(lo, "start") || has_word(lo, "show");
}

static int wants_close(const char *line, const char *lo)
{
    return has(line, "閉じて") || has(line, "とじて") || has(line, "閉じろ") || has(line, "しまって") || has(line, "隠して") || has(line, "かくして") ||
           has_word(lo, "close") || has_word(lo, "hide");
}

/* The first http(s) URL in the line, if any. */
static int url_in(const char *line, char *out, size_t cap)
{
    const char *p = strstr(line, "http://");
    const char *q = strstr(line, "https://");
    if (!p || (q && q < p)) p = q;
    if (!p) return 0;
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && (unsigned char)p[n] < 0x80 && p[n] != '"' && p[n] != '\'' && p[n] != '>' && p[n] != ')') n++;
    while (n > 0 && (p[n - 1] == '.' || p[n - 1] == ',' || p[n - 1] == '!' || p[n - 1] == '?')) n--;
    if (n < 8 || n >= cap) return 0;
    snprintf(out, cap, "%.*s", (int)n, p);
    return 1;
}

/* 「〇〇を調べて」 / "search for 〇〇": the words to search for. */
static int search_in(const char *line, const char *lo, char *out, size_t cap)
{
    static const char *const tails_ja[] = { "を調べて", "をしらべて", "を検索して", "を検索", "を探して", "をさがして", "で検索して", "で調べて", "をググって", NULL };
    size_t n = strlen(line);
    for (int i = 0; tails_ja[i]; i++) {
        size_t t = strlen(tails_ja[i]);
        if (n > t && memcmp(line + n - t, tails_ja[i], t) == 0) { snprintf(out, cap, "%.*s", (int)(n - t), line); return 1; }
    }
    static const char *const heads_en[] = { "search for ", "search the web for ", "look up ", "google ", "search ", NULL };
    for (int i = 0; heads_en[i]; i++) {
        size_t h = strlen(heads_en[i]);
        if (strncmp(lo, heads_en[i], h) == 0 && line[h]) { snprintf(out, cap, "%s", line + h); return 1; }
    }
    return 0;
}

static void url_encode(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
    }
    out[o] = '\0';
}

/* The request to the desktop, appended to the reply. */
static void ask_desktop(char *out, size_t cap, const char *cmd, int app, const char *arg)
{
    size_t o = strlen(out);
    if (arg && *arg) snprintf(out + o, cap - o, "\x1b]7700;%s;%s;%s\x1b\\", cmd, app_id[app], arg);
    else snprintf(out + o, cap - o, "\x1b]7700;%s;%s\x1b\\", cmd, app_id[app]);
}

static int handle_window(enum brain_lang lang, const char *you, const char *line, const char *lo, char *out, size_t cap, struct apps_result *r)
{
    int en = lang == BRAIN_EN;
    int screen = getenv("ALTEREGO_SCREEN") != NULL;
    char url[2048], words[512], q[1600];
    int app = app_in(line, lo);
    int open = wants_open(line, lo), close = wants_close(line, lo);
    int have_url = url_in(line, url, sizeof url);
    (void)you;

    /* a URL: 「https://… を開いて」 or just the URL */
    if (have_url && (open || app == APP_WEB || app == APP_NONE)) {
        if (!screen) { snprintf(out, cap, en ? "I have no screen here, so I cannot open a browser window. On the desktop I would show %s." : "ここには画面がないから、ブラウザの窓は出せない。画面があれば %s を出すよ。", url); return 1; }
        snprintf(out, cap, en ? "Opening %s in the browser." : "ブラウザで %s を開くね。", url);
        ask_desktop(out, cap, "open", APP_WEB, url);
        snprintf(r->action, sizeof r->action, "open web %.100s", url);
        return 1;
    }
    /* a search: 「〇〇を調べて」 */
    if (app != APP_PAINT && app != APP_SHELL && search_in(line, lo, words, sizeof words)) {
        while (*words == ' ') memmove(words, words + 1, strlen(words));
        size_t n = strlen(words);
        while (n > 0 && words[n - 1] == ' ') words[--n] = '\0';
        if (!n) return 0;
        url_encode(words, q, sizeof q);
        snprintf(url, sizeof url, "https://html.duckduckgo.com/html/?q=%s", q);
        if (!screen) { snprintf(out, cap, en ? "I have no screen here to search in. On the desktop I would look up \"%s\" in the browser." : "ここには画面がないから調べられない。画面があればブラウザで「%s」を調べるよ。", words); return 1; }
        snprintf(out, cap, en ? "Looking up \"%s\" in the browser." : "ブラウザで「%s」を調べるね。", words);
        ask_desktop(out, cap, "open", APP_WEB, url);
        snprintf(r->action, sizeof r->action, "search %.100s", words);
        return 1;
    }
    if (app == APP_NONE || (!open && !close)) return 0;
    if (close && !open) {
        if (!screen) { snprintf(out, cap, en ? "There is no screen here, so nothing to close." : "ここには画面がないから、閉じる窓もない。"); return 1; }
        snprintf(out, cap, en ? "Closing %s." : "%sを閉じるね。", en ? app_en[app] : app_ja[app]);
        ask_desktop(out, cap, "close", app, NULL);
        snprintf(r->action, sizeof r->action, "close %s", app_id[app]);
        return 1;
    }
    if (!screen) { snprintf(out, cap, en ? "I have no screen here, so I cannot open %s. On the desktop, say it again." : "ここには画面がないから、%sは開けない。画面のある時に言って。", en ? app_en[app] : app_ja[app]); return 1; }
    if (app == APP_PAINT) snprintf(out, cap, en ? "Opening paint. Draw something and press 「見せる」 so I can see it." : "ペイントを開くね。描いたら「見せる」を押して、私にも見せて。");
    else if (app == APP_WEB) snprintf(out, cap, en ? "Opening the browser." : "ブラウザを開くね。");
    else snprintf(out, cap, en ? "Opening %s." : "%sを開くね。", en ? app_en[app] : app_ja[app]);
    ask_desktop(out, cap, "open", app, NULL);
    snprintf(r->action, sizeof r->action, "open %s", app_id[app]);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* pictures */

static const struct { uint32_t rgb; const char *ja, *en; } palette[] = {
    { 0x000000, "黒", "black" }, { 0x7F7F7F, "灰色", "gray" }, { 0xE53935, "赤", "red" }, { 0xFB8C00, "オレンジ", "orange" },
    { 0xFDD835, "黄色", "yellow" }, { 0x43A047, "緑", "green" }, { 0x00ACC1, "水色", "cyan" }, { 0x1E88E5, "青", "blue" },
    { 0x8E24AA, "紫", "purple" }, { 0xF06292, "ピンク", "pink" }, { 0x8D6E63, "茶色", "brown" },
};
#define NPAL ((int)(sizeof palette / sizeof palette[0]))

static int nearest(uint32_t c)
{
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255, best = 0;
    long bd = -1;
    for (int i = 0; i < NPAL; i++) {
        int pr = (palette[i].rgb >> 16) & 255, pg = (palette[i].rgb >> 8) & 255, pb = palette[i].rgb & 255;
        long d = (long)(r - pr) * (r - pr) + (long)(g - pg) * (g - pg) + (long)(b - pb) * (b - pb);
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Read a binary PPM (P6, 8 bit). */
static unsigned char *read_ppm(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[3] = "";
    int vals[3], nv = 0;
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) { fclose(f); return NULL; }
    while (nv < 3) {
        int c = fgetc(f);
        if (c == EOF) { fclose(f); return NULL; }
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
        if (isspace(c)) continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[nv]) != 1) { fclose(f); return NULL; }
        nv++;
    }
    fgetc(f);                                            /* the single whitespace after maxval */
    if (vals[0] <= 0 || vals[1] <= 0 || vals[0] > 8192 || vals[1] > 8192 || vals[2] != 255) { fclose(f); return NULL; }
    size_t n = (size_t)vals[0] * vals[1] * 3;
    unsigned char *px = malloc(n);
    if (!px || fread(px, 1, n, f) != n) { free(px); fclose(f); return NULL; }
    fclose(f);
    *w = vals[0];
    *h = vals[1];
    return px;
}

int apps_describe_picture(enum brain_lang lang, const char *path, char *out, size_t cap, char *note, size_t ncap)
{
    int en = lang == BRAIN_EN, w, h;
    unsigned char *px = read_ppm(path, &w, &h);
    if (note) note[0] = '\0';
    if (!px) { snprintf(out, cap, en ? "I cannot read that picture (%s)." : "その絵は読めない(%s)。", path); return -1; }
    long count[NPAL] = { 0 }, ink = 0, sx = 0, sy = 0;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char *p = px + ((size_t)y * w + x) * 3;
            if (p[0] > 235 && p[1] > 235 && p[2] > 235) continue;        /* paper */
            ink++;
            sx += x; sy += y;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            count[nearest(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2])]++;
        }
    }
    struct shapes sh;
    shapes_find(px, w, h, &sh);
    free(px);
    if (ink == 0) {
        snprintf(out, cap, en ? "(looks closely) ... a white sheet. Nothing is drawn on it yet." : "（じっと見る）……真っ白。まだ何も描いていないね。");
        return 0;
    }
    /* the colors, most used first (those with at least 3% of the ink) */
    int order[NPAL];
    for (int i = 0; i < NPAL; i++) order[i] = i;
    for (int i = 1; i < NPAL; i++) for (int j = i; j > 0 && count[order[j]] > count[order[j - 1]]; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    char colors[256] = "";
    int ncol = 0;
    for (int i = 0; i < NPAL && ncol < 4; i++) {
        int k = order[i];
        if (count[k] * 100 < ink * 3) break;
        size_t o = strlen(colors);
        snprintf(colors + o, sizeof colors - o, "%s%s", ncol ? (en ? (i + 1 < NPAL && count[order[i + 1]] * 100 >= ink * 3 && ncol < 3 ? ", " : " and ") : "と") : "", en ? palette[k].en : palette[k].ja);
        ncol++;
    }
    int pct = (int)((ink * 100 + w * (long)h / 2) / (w * (long)h));
    if (pct == 0) pct = 1;
    /* where: the center of the ink in thirds of the sheet */
    int cx = (int)(sx / ink), cy = (int)(sy / ink);
    int col = cx * 3 / w, row = cy * 3 / h;
    static const char *const where_ja[3][3] = { { "左上", "上の方", "右上" }, { "左の方", "真ん中", "右の方" }, { "左下", "下の方", "右下" } };
    static const char *const where_en[3][3] = { { "top left", "top", "top right" }, { "left", "middle", "right" }, { "bottom left", "bottom", "bottom right" } };
    /* how big: the box around the ink against the sheet */
    long box = (long)(x1 - x0 + 1) * (y1 - y0 + 1), sheet = (long)w * h;
    const char *size_ja = box * 2 >= sheet ? "大きく" : box * 8 >= sheet ? "ほどほどの大きさで" : "小さく";
    const char *size_en = box * 2 >= sheet ? "big" : box * 8 >= sheet ? "medium sized" : "small";
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    const char *shape_ja = bw * 2 > bh * 3 ? "横に長く、" : bh * 2 > bw * 3 ? "縦に長く、" : "";
    const char *shape_en = bw * 2 > bh * 3 ? ", wide" : bh * 2 > bw * 3 ? ", tall" : "";
    /* a feeling about it, from the main color and how many there are */
    int main_color = order[0];
    const char *feel_ja = ncol >= 4 ? "にぎやかで楽しい。" : main_color == 2 || main_color == 3 ? "あたたかい感じがする。" : main_color == 7 || main_color == 6 ? "すずしい感じがする。" :
                          main_color == 4 ? "明るいね。" : main_color == 5 ? "のどかだね。" : main_color == 9 || main_color == 8 ? "かわいい。" : main_color == 0 ? "はっきりしていて、かっこいい。" : "やさしい色だね。";
    const char *feel_en = ncol >= 4 ? "Lively and fun." : main_color == 2 || main_color == 3 ? "It feels warm." : main_color == 7 || main_color == 6 ? "It feels cool." :
                          main_color == 4 ? "Bright!" : main_color == 5 ? "Peaceful." : main_color == 9 || main_color == 8 ? "Cute." : main_color == 0 ? "Bold and sharp." : "Gentle colors.";
    /* the shapes: 「丸が 1 つと線が 2 本に見える」; a face gets its own greeting */
    char seen[256] = "", parts[128] = "";
    if (sh.count[SH_FACE]) {
        struct shapes rest = sh;
        rest.count[SH_FACE] = 0;                                   /* the face is announced on its own */
        shapes_words(&rest, lang, seen, sizeof seen);
        if (en) snprintf(parts, sizeof parts, " A face! %d eyes%s.", sh.face_eyes, sh.face_mouth ? " and a mouth" : "");
        else snprintf(parts, sizeof parts, "顔だ！目が %d つ%s。", sh.face_eyes, sh.face_mouth ? "と口がある" : "");
        feel_ja = sh.face_mouth ? "にこにこしてる？　私もうれしい。" : "こっちを見てる。";
        feel_en = sh.face_mouth ? "Is it smiling? That makes me happy too." : "It is looking at me.";
    } else {
        shapes_words(&sh, lang, seen, sizeof seen);
        if (sh.count[SH_HEART]) { feel_ja = "ハート！　もらっていいの？　うれしい。"; feel_en = "A heart! Is it for me? I am happy."; }
    }
    if (en) snprintf(out, cap, "(looks closely) ... a picture in %s.%s%s%s%s It covers about %d%% of the sheet, %s%s, at the %s. %s",
                     colors, parts, seen[0] ? " I see " : "", seen, seen[0] ? "." : "", pct, size_en, shape_en, where_en[row][col], feel_en);
    else snprintf(out, cap, "（じっと見る）……%sの絵だ。%s%s%s紙の %d%% くらい、%sに、%s%s描いてある。%s",
                  colors, parts, seen, seen[0] ? "に見える。" : "", pct, where_ja[row][col], shape_ja, size_ja, feel_ja);
    if (note) {
        if (sh.count[SH_FACE]) snprintf(note, ncap, en ? "you showed me a drawing of a face" : "%sで描いた顔の絵を見せてもらった", colors);
        else if (seen[0]) snprintf(note, ncap, en ? "you showed me a drawing: %s" : "%sの絵(%s)を見せてもらった", en ? seen : colors, seen);
        else snprintf(note, ncap, en ? "you showed me a picture in %s" : "%sの絵を見せてもらった", colors);
    }
    return 0;
}

/* The newest *.ppm in the paint directory of the store. */
static int newest_picture(char *out, size_t cap)
{
    const char *dir = store_open();
    if (!dir) return 0;
    char pdir[300];
    snprintf(pdir, sizeof pdir, "%s/paint", dir);
    DIR *d = opendir(pdir);
    if (!d) return 0;
    struct dirent *e;
    char best[300] = "";
    time_t bt = 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".ppm") != 0) continue;
        char path[600];
        snprintf(path, sizeof path, "%s/%s", pdir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!best[0] || st.st_mtime > bt || (st.st_mtime == bt && strcmp(e->d_name, best) > 0)) { bt = st.st_mtime; snprintf(best, sizeof best, "%s", e->d_name); }
    }
    closedir(d);
    if (!best[0]) return 0;
    snprintf(out, cap, "%s/%s", pdir, best);
    return 1;
}

static int handle_picture(enum brain_lang lang, const char *you, const char *line, const char *lo, char *out, size_t cap, struct apps_result *r)
{
    int en = lang == BRAIN_EN;
    char path[600] = "";
    const char *p = strstr(line, "絵を見て: ");
    if (!p) p = strstr(line, "絵を見て:");
    int asked = p != NULL || has(line, "絵を見て") || has(line, "絵、見て") || has(line, "絵をみて") || has(line, "絵見て") || has(line, "描いた絵") || has(line, "かいた絵") ||
                has(line, "絵を見せ") || has(line, "絵、どう") || has(line, "絵はどう") || has(line, "絵を描いた") || has(line, "絵をかいた") ||
                has(lo, "look at my drawing") || has(lo, "look at my picture") || has(lo, "see my drawing") || has(lo, "see my picture") || has(lo, "my drawing") || has(lo, "my painting");
    if (!asked) return 0;
    /* a path given with the request (the desktop's 「見せる」 does this) */
    const char *s = strchr(line, '/');
    if (s) {
        size_t n = strcspn(s, " \t\r\n");
        if (n > 4 && n < sizeof path && strncmp(s + n - 4, ".ppm", 4) == 0) snprintf(path, sizeof path, "%.*s", (int)n, s);
    }
    if (!path[0] && !newest_picture(path, sizeof path)) {
        snprintf(out, cap, en ? "I have no picture from you yet. Open paint, draw something and press 「見せる」, %s." : "まだ絵をもらっていない。ペイントで描いて「見せる」を押してね、%s。", you);
        return 1;
    }
    apps_describe_picture(lang, path, out, cap, r->note, sizeof r->note);
    snprintf(r->action, sizeof r->action, "look at %.100s", path);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* drawing: 「顔を描いて」 */

/* What it knows how to draw: a spec for paint_run (see paint.h: units of
 * 1/1000 of the sheet's height, the sheet about 1600 wide). */
static const struct { const char *ja, *ja2, *en, *en2, *spec; } drawings[] = {
    { "丸", "円", "circle", "round", "c 800 500 300" },
    { "線", "直線", "line", NULL, "l 300 500 1300 500" },
    { "四角", "しかく", "square", "rectangle", "p 5 500 250 1100 250 1100 750 500 750 500 250" },
    { "三角", "さんかく", "triangle", NULL, "p 4 800 200 1150 800 450 800 800 200" },
    { "星", "ほし", "star", NULL, "p 6 800 150 1006 783 467 392 1133 392 594 783 800 150" },
    { "ハート", "はーと", "heart", NULL, "p 17 800 420 818 366 913 288 1052 313 1120 440 1052 586 913 712 818 816 800 860 782 816 687 712 548 586 480 440 548 313 687 288 782 366 800 420" },
    { "顔", "かお", "face", "smiley", "c 800 500 300 C 690 420 30 C 910 420 30 p 5 660 600 720 660 800 680 880 660 940 600" },
    { "太陽", "たいよう", "sun", NULL, "k FB8C00 C 800 500 200 s 8 l 800 200 800 100 l 800 800 800 900 l 500 500 400 500 l 1100 500 1200 500 l 588 288 517 217 l 1012 288 1083 217 l 588 712 517 783 l 1012 712 1083 783" },
    { "家", "いえ", "house", "home", "k 8D6E63 P 4 500 500 1100 500 1100 900 500 900 k E53935 P 3 450 500 800 200 1150 500 k 1E88E5 P 4 740 650 860 650 860 900 740 900" },
    { "木", "き", "tree", NULL, "k 8D6E63 P 4 740 600 860 600 860 950 740 950 k 43A047 C 800 450 250" },
    { "花", "はな", "flower", NULL, "k 43A047 s 6 l 800 600 800 950 k F06292 C 800 300 90 C 930 390 90 C 670 390 90 C 720 540 90 C 880 540 90 k FDD835 C 800 430 70" },
    { "山", "やま", "mountain", NULL, "k 7F7F7F P 3 250 900 800 200 1350 900 k FFFFFF P 3 690 350 800 200 910 350" },
    { "猫", "ねこ", "cat", NULL, "c 800 520 280 P 3 580 330 620 120 760 270 P 3 1020 330 980 120 840 270 C 700 470 25 C 900 470 25 P 3 770 580 830 580 800 620 l 500 560 720 600 l 500 640 720 620 l 1100 560 880 600 l 1100 640 880 620" },
    { "魚", "さかな", "fish", NULL, "p 13 1050 500 1010 585 900 647 750 670 600 647 490 585 450 500 490 415 600 353 750 330 900 353 1010 415 1050 500 P 3 1050 500 1250 350 1250 650 C 560 450 20" },
    { "車", "くるま", "car", NULL, "P 4 400 500 1200 500 1200 750 400 750 P 4 550 500 700 350 950 350 1050 500 k 000000 C 600 780 70 C 1000 780 70" },
    { NULL, NULL, NULL, NULL, NULL }
};

static const struct { const char *ja, *en, *hex; } draw_colors[] = {
    { "赤", "red", "E53935" }, { "青", "blue", "1E88E5" }, { "緑", "green", "43A047" }, { "黄色", "yellow", "FDD835" }, { "黄", "yellow", "FDD835" },
    { "黒", "black", "000000" }, { "紫", "purple", "8E24AA" }, { "ピンク", "pink", "F06292" }, { "桃色", "pink", "F06292" }, { "水色", "cyan", "00ACC1" },
    { "オレンジ", "orange", "FB8C00" }, { "橙", "orange", "FB8C00" }, { "茶色", "brown", "8D6E63" }, { "灰色", "gray", "7F7F7F" }, { NULL, NULL, NULL }
};

static int handle_draw(enum brain_lang lang, const char *you, const char *line, const char *lo, char *out, size_t cap, struct apps_result *r)
{
    int en = lang == BRAIN_EN;
    int asked = has(line, "描いて") || has(line, "かいて") || has(line, "描け") || has(line, "書いて") ||
                has_word(lo, "draw") || (has_word(lo, "paint") && (has(lo, "paint a ") || has(lo, "paint me ")));
    if (!asked || strchr(line, '/')) return 0;                     /* 「/path に書いて」 is a file */
    int pick = -1, any = has(line, "何か") || has(line, "なにか") || has(line, "好きなもの") || has(lo, "something") || has(lo, "anything");
    for (int i = 0; drawings[i].ja && pick < 0; i++)
        if (has(line, drawings[i].ja) || (drawings[i].ja2 && has(line, drawings[i].ja2)) || has_word(lo, drawings[i].en) || (drawings[i].en2 && has_word(lo, drawings[i].en2))) pick = i;
    if (pick < 0 && !any) {
        if (!has(line, "絵") && !has(line, "描いて") && !has(line, "描け") && !has_word(lo, "draw")) return 0;      /* 「〇〇と書いて」 without a thing to draw */
        size_t o = (size_t)snprintf(out, cap, en ? "I cannot draw that yet. What I can draw: " : "それはまだ描けない。描けるのは: ");
        for (int i = 0; drawings[i].ja && o + 16 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? (en ? ", " : "、") : "", en ? drawings[i].en : drawings[i].ja);
        if (o + 4 < cap) snprintf(out + o, cap - o, en ? "." : "。");
        return 1;
    }
    if (pick < 0) pick = (int)(time(NULL) % 15);
    const char *color = NULL, *color_name = NULL;
    for (int i = 0; draw_colors[i].ja && !color; i++)
        if (has(line, draw_colors[i].ja) || has_word(lo, draw_colors[i].en)) { color = draw_colors[i].hex; color_name = en ? draw_colors[i].en : draw_colors[i].ja; }
    const char *name = en ? drawings[pick].en : drawings[pick].ja;
    if (!getenv("ALTEREGO_SCREEN")) {
        if (en) snprintf(out, cap, "I have no screen here to draw on. On the desktop I would draw %s%s%s in paint.", color_name ? color_name : "a", " ", name);
        else snprintf(out, cap, "ここには画面がないから描けない。画面があればペイントに%s%s%sを描くよ。", color_name ? color_name : "", color_name ? "の" : "", name);
        return 1;
    }
    char spec[900];
    snprintf(spec, sizeof spec, "%s%s%s%s", color ? "k " : "", color ? color : "", color ? " " : "", drawings[pick].spec);
    if (en) snprintf(out, cap, "I drew %s%s%s in paint. Have a look, %s.", color_name ? color_name : "a", color_name ? " " : " ", name, you);
    else snprintf(out, cap, "%s%s%sを描いたよ。ペイントを見て、%s。", color_name ? color_name : "", color_name ? "の" : "", name, you);
    ask_desktop(out, cap, "draw", APP_PAINT, spec);
    snprintf(r->action, sizeof r->action, "draw %s", drawings[pick].en);
    snprintf(r->note, sizeof r->note, en ? "I drew %s for you" : "%sに%sを描いてあげた", en ? drawings[pick].en : you, drawings[pick].ja);
    return 1;
}

int apps_handle(enum brain_lang lang, const char *you, const char *line, const char *lo, char *out, size_t cap, struct apps_result *r)
{
    memset(r, 0, sizeof *r);
    if (handle_picture(lang, you, line, lo, out, cap, r)) return 1;
    if (handle_draw(lang, you, line, lo, out, cap, r)) return 1;
    return handle_window(lang, you, line, lo, out, cap, r);
}

/* The paint app. See paint.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "paint.h"

#define C_TB      0x2B3340
#define C_TBTEXT  0xDDE3EA
#define C_TBBTN   0x3C4756
#define C_TBON    0x3B6EA5
#define C_PAPER   0xFFFFFF
#define C_FRAME   0x8A94A0

const uint32_t paint_palette[PAINT_COLORS] = {
    0x000000, 0x7F7F7F, 0xE53935, 0xFB8C00, 0xFDD835, 0x43A047,
    0x00ACC1, 0x1E88E5, 0x8E24AA, 0xF06292, 0x8D6E63, 0xFFFFFF };

/* Toolbar items, laid out left to right. */
enum { IT_COLOR = 0, IT_SIZE = PAINT_COLORS, IT_ERASER = IT_SIZE + 3, IT_CLEAR, IT_SAVE, IT_SHOW, IT_MAX };
struct item { int x, w; };

static void layout(const struct font *f, struct item it[IT_MAX])
{
    int x = 6;
    for (int i = 0; i < PAINT_COLORS; i++) { it[IT_COLOR + i].x = x; it[IT_COLOR + i].w = 18; x += 20; }
    x += 8;
    static const char *const sizes[3] = { "細", "中", "太" };
    for (int i = 0; i < 3; i++) { int w = font_text_width(f, sizes[i]) + 10; it[IT_SIZE + i].x = x; it[IT_SIZE + i].w = w; x += w + 4; }
    x += 8;
    it[IT_ERASER].x = x; it[IT_ERASER].w = font_text_width(f, "消しゴム") + 12; x += it[IT_ERASER].w + 6;
    it[IT_CLEAR].x = x; it[IT_CLEAR].w = font_text_width(f, "全部消す") + 12; x += it[IT_CLEAR].w + 6;
    it[IT_SAVE].x = x; it[IT_SAVE].w = font_text_width(f, "保存") + 12; x += it[IT_SAVE].w + 6;
    it[IT_SHOW].x = x; it[IT_SHOW].w = font_text_width(f, "見せる") + 12;
}

int paint_init(struct paint *p, int w, int h)
{
    memset(p, 0, sizeof *p);
    p->w = w;
    p->h = h;
    p->px = malloc((size_t)w * h * sizeof *p->px);
    if (!p->px) return -1;
    for (size_t i = 0; i < (size_t)w * h; i++) p->px[i] = C_PAPER;
    p->color = paint_palette[0];
    p->size = 3;
    snprintf(p->msg, sizeof p->msg, "マウスで描く");
    return 0;
}

void paint_free(struct paint *p)
{
    free(p->px);
    p->px = NULL;
}

int paint_width(const struct paint *p) { return p->w; }
int paint_height(const struct paint *p) { return PAINT_TB_H + p->h; }

uint32_t paint_pixel(const struct paint *p, int x, int y)
{
    if (x < 0 || y < 0 || x >= p->w || y >= p->h) return 0;
    return p->px[(size_t)y * p->w + x];
}

void paint_clear(struct paint *p)
{
    for (size_t i = 0; i < (size_t)p->w * p->h; i++) p->px[i] = C_PAPER;
    p->strokes = 0;
    snprintf(p->msg, sizeof p->msg, "全部消した");
}

static void dot(struct paint *p, int cx, int cy)
{
    uint32_t c = p->tool == PT_ERASER ? C_PAPER : p->color;
    int r = p->size;
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= p->h) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= p->w) continue;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
            p->px[(size_t)y * p->w + x] = c;
        }
    }
}

static void line(struct paint *p, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        dot(p, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int paint_press(struct paint *p, const struct font *f, int x, int y, const char *save_dir)
{
    if (y < PAINT_TB_H) {
        struct item it[IT_MAX];
        layout(f, it);
        for (int i = 0; i < IT_MAX; i++) {
            if (x < it[i].x || x >= it[i].x + it[i].w) continue;
            if (i == IT_SHOW) {
                if (p->strokes == 0) { snprintf(p->msg, sizeof p->msg, "まだ何も描いていない"); return PAINT_NONE; }
                if (paint_save(p, save_dir) != 0) return PAINT_NONE;
                snprintf(p->msg, sizeof p->msg, "AlterEgo に見せた");
                return PAINT_SHOW;
            }
            if (i < IT_SIZE) { p->color = paint_palette[i]; p->tool = PT_PEN; snprintf(p->msg, sizeof p->msg, "色を選んだ"); }
            else if (i < IT_ERASER) { p->size = i == IT_SIZE ? 1 : i == IT_SIZE + 1 ? 3 : 6; snprintf(p->msg, sizeof p->msg, "太さ: %s", i == IT_SIZE ? "細" : i == IT_SIZE + 1 ? "中" : "太"); }
            else if (i == IT_ERASER) { p->tool = p->tool == PT_ERASER ? PT_PEN : PT_ERASER; snprintf(p->msg, sizeof p->msg, p->tool == PT_ERASER ? "消しゴム" : "ペン"); }
            else if (i == IT_CLEAR) paint_clear(p);
            else if (i == IT_SAVE) paint_save(p, save_dir);
            return PAINT_NONE;
        }
        return PAINT_NONE;
    }
    y -= PAINT_TB_H;
    if (x < 0 || y < 0 || x >= p->w || y >= p->h) return PAINT_NONE;
    p->drawing = 1;
    p->lx = x;
    p->ly = y;
    dot(p, x, y);
    p->strokes++;
    return PAINT_NONE;
}

void paint_move(struct paint *p, int x, int y)
{
    if (!p->drawing) return;
    y -= PAINT_TB_H;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= p->w) x = p->w - 1;
    if (y >= p->h) y = p->h - 1;
    line(p, p->lx, p->ly, x, y);
    p->lx = x;
    p->ly = y;
}

void paint_release(struct paint *p) { p->drawing = 0; }

int paint_save(struct paint *p, const char *dir)
{
    if (!dir || !*dir) { snprintf(p->msg, sizeof p->msg, "保存先がない"); return -1; }
    mkdir(dir, 0755);
    char path[512];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(path, sizeof path, "%s/paint-%04d%02d%02d-%02d%02d%02d.ppm", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(p->msg, sizeof p->msg, "保存できない: %s", strerror(errno)); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", p->w, p->h);
    for (size_t i = 0; i < (size_t)p->w * p->h; i++) {
        uint32_t c = p->px[i];
        fputc((c >> 16) & 0xFF, f); fputc((c >> 8) & 0xFF, f); fputc(c & 0xFF, f);
    }
    int rc = fclose(f) == 0 ? 0 : -1;
    if (rc == 0) { snprintf(p->last, sizeof p->last, "%s", path); snprintf(p->msg, sizeof p->msg, "保存した: %.100s", path); }
    else snprintf(p->msg, sizeof p->msg, "保存できない: %s", strerror(errno));
    return rc;
}

/* ---- drawing on request (paint_run) */

static int U(const struct paint *p, long v) { return (int)(v * p->h / 1000); }

static void poly_fill(struct paint *p, const int *xs, const int *ys, int n)
{
    int ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) { if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
    if (ymin < 0) ymin = 0;
    if (ymax >= p->h) ymax = p->h - 1;
    for (int y = ymin; y <= ymax; y++) {
        int xs_[64], m = 0;
        for (int i = 0; i < n && m < 64; i++) {
            int j = (i + 1) % n;
            int y0 = ys[i], y1 = ys[j], x0 = xs[i], x1 = xs[j];
            if (y0 == y1) continue;
            if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) xs_[m++] = x0 + (int)((long)(y - y0) * (x1 - x0) / (y1 - y0));
        }
        for (int i = 1; i < m; i++) for (int j = i; j > 0 && xs_[j] < xs_[j - 1]; j--) { int t = xs_[j]; xs_[j] = xs_[j - 1]; xs_[j - 1] = t; }
        for (int i = 0; i + 1 < m; i += 2) {
            int a = xs_[i] < 0 ? 0 : xs_[i], b = xs_[i + 1] >= p->w ? p->w - 1 : xs_[i + 1];
            for (int x = a; x <= b; x++) p->px[(size_t)y * p->w + x] = p->color;
        }
    }
}

/* Paste the ink of the PPM at `path` (paper = white stays out) into the
 * box x0,y0-x1,y1 of the sheet, the picture's own ink box scaled to it. */
static int paste(struct paint *p, const char *path, int x0, int y0, int x1, int y1)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[3] = "";
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 || strcmp(magic, "P6") != 0 || w <= 0 || h <= 0 || w > 8192 || h > 8192) { fclose(f); return 0; }
    fgetc(f);
    size_t n = (size_t)w * h * 3;
    unsigned char *px = malloc(n);
    if (!px || fread(px, 1, n, f) != n) { free(px); fclose(f); return 0; }
    fclose(f);
    int ix0 = w, iy0 = h, ix1 = -1, iy1 = -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const unsigned char *q = px + ((size_t)y * w + x) * 3;
        if (q[0] > 235 && q[1] > 235 && q[2] > 235) continue;
        if (x < ix0) ix0 = x;
        if (x > ix1) ix1 = x;
        if (y < iy0) iy0 = y;
        if (y > iy1) iy1 = y;
    }
    if (ix1 < 0) { free(px); return 0; }
    int iw = ix1 - ix0 + 1, ih = iy1 - iy0 + 1, dw = x1 - x0, dh = y1 - y0;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= p->h) continue;
        int sy = iy0 + (int)((long)(y - y0) * ih / dh);
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= p->w) continue;
            int sx = ix0 + (int)((long)(x - x0) * iw / dw);
            const unsigned char *q = px + ((size_t)sy * w + sx) * 3;
            if (q[0] > 235 && q[1] > 235 && q[2] > 235) continue;
            p->px[(size_t)y * p->w + x] = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
        }
    }
    free(px);
    return 1;
}

int paint_run(struct paint *p, const char *spec)
{
    int done = 0, drawn = 0, tool = p->tool, size = p->size;
    uint32_t color = p->color;
    p->tool = PT_PEN;
    const char *s = spec;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        char cmd = *s++;
        long v[64];
        int nv = 0;
        if (cmd == 'k') { unsigned long c = strtoul(s, (char **)&s, 16); p->color = (uint32_t)(c & 0xFFFFFF); done++; continue; }
        if (cmd == 'e') { paint_clear(p); done++; drawn = 0; continue; }
        if (cmd == 'i') {                                               /* i path x0 y0 x1 y1 */
            while (*s == ' ') s++;
            char path[512];
            size_t pl = strcspn(s, " ");
            if (pl == 0 || pl >= sizeof path) return done;
            snprintf(path, sizeof path, "%.*s", (int)pl, s);
            s += pl;
            for (int i = 0; i < 4; i++) { char *end; v[i] = strtol(s, &end, 10); if (end == s) return done; s = end; }
            if (paste(p, path, U(p, v[0]), U(p, v[1]), U(p, v[2]), U(p, v[3]))) drawn = 1;
            done++;
            continue;
        }
        int want = cmd == 's' ? 1 : cmd == 'l' ? 4 : (cmd == 'c' || cmd == 'C') ? 3 : (cmd == 'p' || cmd == 'P') ? 1 : -1;
        if (want < 0) break;                                            /* unknown: stop */
        for (int i = 0; i < want; i++) { char *end; v[nv] = strtol(s, &end, 10); if (end == s) return done; s = end; nv++; }
        if (cmd == 'p' || cmd == 'P') {
            int n = (int)v[0];
            if (n < 2 || n > 30) return done;
            int xs[30], ys[30];
            for (int i = 0; i < n; i++) {
                char *end;
                long x = strtol(s, &end, 10); if (end == s) return done; s = end;
                long y = strtol(s, &end, 10); if (end == s) return done; s = end;
                xs[i] = U(p, x); ys[i] = U(p, y);
            }
            drawn = 1;
            if (cmd == 'P') poly_fill(p, xs, ys, n);
            for (int i = 0; i + 1 < n; i++) line(p, xs[i], ys[i], xs[i + 1], ys[i + 1]);
            if (cmd == 'P') line(p, xs[n - 1], ys[n - 1], xs[0], ys[0]);
        } else if (cmd == 's') { p->size = U(p, v[0]); if (p->size < 1) p->size = 1; if (p->size > 40) p->size = 40; }
        else if (cmd == 'l') { line(p, U(p, v[0]), U(p, v[1]), U(p, v[2]), U(p, v[3])); drawn = 1; }
        else if (cmd == 'c' || cmd == 'C') {
            int cx = U(p, v[0]), cy = U(p, v[1]), r = U(p, v[2]);
            drawn = 1;
            if (cmd == 'C') { int keep = p->size; p->size = r; dot(p, cx, cy); p->size = keep; done++; continue; }
            int px0 = cx + r, py0 = cy;
            for (int a = 10; a <= 360; a += 10) {
                double rad = a * 3.14159265 / 180;
                int x = cx + (int)(r * cos(rad) + (cos(rad) < 0 ? -0.5 : 0.5)), y = cy + (int)(r * sin(rad) + (sin(rad) < 0 ? -0.5 : 0.5));
                line(p, px0, py0, x, y);
                px0 = x; py0 = y;
            }
        }
        done++;
    }
    p->tool = tool;
    p->size = size;
    p->color = color;
    if (drawn) p->strokes++;
    return done;
}

void paint_draw(const struct paint *p, struct canvas *c, const struct font *f, int ox, int oy)
{
    struct item it[IT_MAX];
    layout(f, it);
    canvas_fill(c, ox, oy, p->w, PAINT_TB_H, C_TB);
    for (int i = 0; i < PAINT_COLORS; i++) {
        int on = p->tool == PT_PEN && p->color == paint_palette[i];
        canvas_fill(c, ox + it[i].x, oy + 5, it[i].w, PAINT_TB_H - 10, paint_palette[i]);
        canvas_frame(c, ox + it[i].x, oy + 5, it[i].w, PAINT_TB_H - 10, on ? C_TBTEXT : C_FRAME);
        if (on) canvas_frame(c, ox + it[i].x + 1, oy + 6, it[i].w - 2, PAINT_TB_H - 12, C_TBTEXT);
    }
    static const char *const labels[] = { "細", "中", "太", "消しゴム", "全部消す", "保存", "見せる" };
    static const int sizes[3] = { 1, 3, 6 };
    for (int i = IT_SIZE; i < IT_MAX; i++) {
        int on = (i < IT_ERASER && p->size == sizes[i - IT_SIZE]) || (i == IT_ERASER && p->tool == PT_ERASER);
        canvas_fill(c, ox + it[i].x, oy + 4, it[i].w, PAINT_TB_H - 8, on ? C_TBON : C_TBBTN);
        font_text(f, c, ox + it[i].x + 5, oy + 6, labels[i - IT_SIZE], C_TBTEXT, on ? C_TBON : C_TBBTN);
    }
    for (int y = 0; y < p->h; y++) {
        int cy = oy + PAINT_TB_H + y;
        if (cy < 0 || cy >= c->h) continue;
        for (int x = 0; x < p->w; x++) {
            int cx = ox + x;
            if (cx < 0 || cx >= c->w) continue;
            c->px[(size_t)cy * c->w + cx] = p->px[(size_t)y * p->w + x];
        }
    }
}

/* The paint app. See paint.h. */
#define _GNU_SOURCE
#include <errno.h>
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
enum { IT_COLOR = 0, IT_SIZE = PAINT_COLORS, IT_ERASER = IT_SIZE + 3, IT_CLEAR, IT_SAVE, IT_MAX };
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
    it[IT_SAVE].x = x; it[IT_SAVE].w = font_text_width(f, "保存") + 12;
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

void paint_press(struct paint *p, const struct font *f, int x, int y, const char *save_dir)
{
    if (y < PAINT_TB_H) {
        struct item it[IT_MAX];
        layout(f, it);
        for (int i = 0; i < IT_MAX; i++) {
            if (x < it[i].x || x >= it[i].x + it[i].w) continue;
            if (i < IT_SIZE) { p->color = paint_palette[i]; p->tool = PT_PEN; snprintf(p->msg, sizeof p->msg, "色を選んだ"); }
            else if (i < IT_ERASER) { p->size = i == IT_SIZE ? 1 : i == IT_SIZE + 1 ? 3 : 6; snprintf(p->msg, sizeof p->msg, "太さ: %s", i == IT_SIZE ? "細" : i == IT_SIZE + 1 ? "中" : "太"); }
            else if (i == IT_ERASER) { p->tool = p->tool == PT_ERASER ? PT_PEN : PT_ERASER; snprintf(p->msg, sizeof p->msg, p->tool == PT_ERASER ? "消しゴム" : "ペン"); }
            else if (i == IT_CLEAR) paint_clear(p);
            else if (i == IT_SAVE) paint_save(p, save_dir);
            return;
        }
        return;
    }
    y -= PAINT_TB_H;
    if (x < 0 || y < 0 || x >= p->w || y >= p->h) return;
    p->drawing = 1;
    p->lx = x;
    p->ly = y;
    dot(p, x, y);
    p->strokes++;
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
    if (rc == 0) snprintf(p->msg, sizeof p->msg, "保存した: %.100s", path);
    else snprintf(p->msg, sizeof p->msg, "保存できない: %s", strerror(errno));
    return rc;
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
    static const char *const labels[] = { "細", "中", "太", "消しゴム", "全部消す", "保存" };
    static const int sizes[3] = { 1, 3, 6 };
    for (int i = IT_SIZE; i < IT_MAX; i++) {
        int on = (i < IT_ERASER && p->size == sizes[i - IT_SIZE]) || (i == IT_ERASER && p->tool == PT_ERASER);
        canvas_fill(c, ox + it[i].x, oy + 4, it[i].w, PAINT_TB_H - 8, on ? C_TBON : C_TBBTN);
        font_text(f, c, ox + it[i].x + 5, oy + 6, labels[i - IT_SIZE], C_TBTEXT, on ? C_TBON : C_TBBTN);
    }
    int mx = it[IT_SAVE].x + it[IT_SAVE].w + 10;
    if (mx < p->w - 8) {
        char shown[128];
        snprintf(shown, sizeof shown, "%s", p->msg);
        while (shown[0] && font_text_width(f, shown) > p->w - mx - 4) {       /* cut whole UTF-8 characters off the end */
            size_t n = strlen(shown) - 1;
            while (n > 0 && ((unsigned char)shown[n] & 0xC0) == 0x80) n--;
            shown[n] = '\0';
        }
        font_text(f, c, ox + mx, oy + 6, shown, C_TBTEXT, C_TB);
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

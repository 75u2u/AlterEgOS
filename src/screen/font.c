#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "../agent/utf8.h"

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int font_load(struct font *f, const char *path)
{
    memset(f, 0, sizeof *f);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int cap = 8192;
    f->bits = malloc((size_t)cap * 32);
    f->wide = malloc((size_t)cap);
    f->index = calloc(65536, sizeof *f->index);
    if (!f->bits || !f->wide || !f->index) { fclose(fp); font_free(f); return -1; }
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        unsigned long cp = strtoul(line, NULL, 16);
        if (cp >= 65536) continue;
        char *hex = colon + 1;
        size_t len = strcspn(hex, "\r\n");
        if (len != 32 && len != 64) continue;
        if (f->n == cap) {
            cap *= 2;
            uint8_t *nb = realloc(f->bits, (size_t)cap * 32);
            uint8_t *nw = realloc(f->wide, (size_t)cap);
            if (!nb || !nw) { free(nb ? nb : f->bits); fclose(fp); font_free(f); return -1; }
            f->bits = nb;
            f->wide = nw;
        }
        uint8_t *g = f->bits + (size_t)f->n * 32;
        memset(g, 0, 32);
        int bytes_per_row = len == 64 ? 2 : 1;
        for (int row = 0; row < 16; row++) {
            for (int b = 0; b < bytes_per_row; b++) {
                int hi = hexval(hex[(row * bytes_per_row + b) * 2]);
                int lo = hexval(hex[(row * bytes_per_row + b) * 2 + 1]);
                if (hi < 0 || lo < 0) { hi = lo = 0; }
                g[row * 2 + b] = (uint8_t)(hi << 4 | lo);
            }
        }
        f->wide[f->n] = bytes_per_row == 2;
        f->index[cp] = (uint16_t)(f->n + 1);
        f->n++;
    }
    fclose(fp);
    return f->n;
}

void font_free(struct font *f)
{
    free(f->bits);
    free(f->wide);
    free(f->index);
    memset(f, 0, sizeof *f);
}

/* Without a glyph, guess from the script: East Asian blocks are wide. */
static int guess_wide(uint32_t cp)
{
    if (cp >= 0xFF61 && cp <= 0xFF9F) return 0;        /* half-width katakana */
    if (cp >= 0x1100 && cp <= 0x115F) return 1;
    if (cp >= 0x2E80 && cp <= 0xA4CF) return 1;
    if (cp >= 0xAC00 && cp <= 0xD7A3) return 1;
    if (cp >= 0xF900 && cp <= 0xFAFF) return 1;
    if (cp >= 0xFE30 && cp <= 0xFE4F) return 1;
    if (cp >= 0xFF00 && cp <= 0xFF60) return 1;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 1;
    if (cp >= 0x20000 && cp <= 0x3FFFD) return 1;
    return 0;
}

int font_cells(const struct font *f, uint32_t cp)
{
    if (cp < 65536 && f->index[cp]) return f->wide[f->index[cp] - 1] ? 2 : 1;
    return guess_wide(cp) ? 2 : 1;
}

void font_draw(const struct font *f, struct canvas *c, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg)
{
    int cells = font_cells(f, cp);
    int w = cells * FONT_W;
    canvas_fill(c, x, y, w, FONT_H, bg);
    const uint8_t *g = (cp < 65536 && f->index[cp]) ? f->bits + (size_t)(f->index[cp] - 1) * 32 : NULL;
    if (!g) {
        if (cp > ' ') canvas_frame(c, x + 1, y + 2, w - 2, FONT_H - 4, fg);
        return;
    }
    for (int row = 0; row < FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= c->h) continue;
        uint32_t bitsrow = cells == 2 ? ((uint32_t)g[row * 2] << 8 | g[row * 2 + 1]) : g[row * 2];
        if (!bitsrow) continue;
        uint32_t *line = c->px + (size_t)py * c->w;
        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < 0 || px >= c->w) continue;
            if (bitsrow & (1u << (w - 1 - col))) line[px] = fg;
        }
    }
}

int font_text(const struct font *f, struct canvas *c, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    uint32_t cp;
    size_t k;
    while ((k = utf8_decode(s, &cp)) > 0) {
        font_draw(f, c, x, y, cp, fg, bg);
        x += font_cells(f, cp) * FONT_W;
        s += k;
    }
    return x;
}

int font_text_width(const struct font *f, const char *s)
{
    int w = 0;
    uint32_t cp;
    size_t k;
    while ((k = utf8_decode(s, &cp)) > 0) { w += font_cells(f, cp) * FONT_W; s += k; }
    return w;
}

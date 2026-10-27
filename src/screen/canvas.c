#include <stdlib.h>
#include <string.h>

#include "canvas.h"

int canvas_init(struct canvas *c, int w, int h)
{
    c->w = w;
    c->h = h;
    c->px = calloc((size_t)w * (size_t)h, sizeof *c->px);
    return c->px ? 0 : -1;
}

void canvas_free(struct canvas *c)
{
    free(c->px);
    c->px = NULL;
}

void canvas_fill(struct canvas *c, int x, int y, int w, int h, uint32_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + (size_t)j * c->w;
        for (int i = x0; i < x1; i++) row[i] = color;
    }
}

void canvas_hline(struct canvas *c, int x, int y, int w, uint32_t color) { canvas_fill(c, x, y, w, 1, color); }

void canvas_frame(struct canvas *c, int x, int y, int w, int h, uint32_t color)
{
    canvas_fill(c, x, y, w, 1, color);
    canvas_fill(c, x, y + h - 1, w, 1, color);
    canvas_fill(c, x, y, 1, h, color);
    canvas_fill(c, x + w - 1, y, 1, h, color);
}

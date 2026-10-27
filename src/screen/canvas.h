#ifndef ALTEREGO_CANVAS_H
#define ALTEREGO_CANVAS_H

#include <stdint.h>

/* An off-screen image, 32-bit 0x00RRGGBB per pixel, drawn into by the
 * compositor and copied to the framebuffer once per frame. */
struct canvas {
    uint32_t *px;
    int w, h;
};

int  canvas_init(struct canvas *c, int w, int h);
void canvas_free(struct canvas *c);
void canvas_fill(struct canvas *c, int x, int y, int w, int h, uint32_t color);
void canvas_frame(struct canvas *c, int x, int y, int w, int h, uint32_t color);   /* 1 px border */
void canvas_hline(struct canvas *c, int x, int y, int w, uint32_t color);

#endif

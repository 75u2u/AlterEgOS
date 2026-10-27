#ifndef ALTEREGO_FONT_H
#define ALTEREGO_FONT_H

#include <stdint.h>

#include "canvas.h"

/* A 16 px bitmap font in Unifont's .hex text format: half-width glyphs are
 * 8x16, full-width (kana, kanji) 16x16. Loaded once at start from
 * /usr/share/alterego/font.hex. Code points without a glyph are drawn as
 * a hollow box of the width the code point's script suggests. */
#define FONT_H 16
#define FONT_W 8                /* one cell; wide glyphs take two */

struct font {
    uint8_t *bits;              /* 32 bytes per glyph: 16 rows x 2 bytes, MSB left */
    uint8_t *wide;              /* 1 if the glyph is 16 px wide */
    uint16_t *index;            /* code point -> glyph number + 1, 0 = none (BMP only) */
    int n;
};

int  font_load(struct font *f, const char *path);      /* glyph count, -1 on error */
void font_free(struct font *f);
int  font_cells(const struct font *f, uint32_t cp);     /* 1 or 2 */
void font_draw(const struct font *f, struct canvas *c, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg);
/* Draw a UTF-8 string; returns the x after the last glyph. */
int  font_text(const struct font *f, struct canvas *c, int x, int y, const char *s, uint32_t fg, uint32_t bg);
int  font_text_width(const struct font *f, const char *s);

#endif

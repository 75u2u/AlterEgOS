#ifndef ALTEREGO_PAINT_H
#define ALTEREGO_PAINT_H

#include <stdint.h>

#include "canvas.h"
#include "font.h"

/* The paint app: a bitmap drawn into with the mouse, under a toolbar with
 * a palette, three brush sizes, an eraser, clear, save and 「見せる」 (show
 * the picture to AlterEgo: saved, then handed to the chat). It lives inside
 * the desktop (no process of its own): the screen hands it mouse events
 * in its own coordinates and asks it to draw itself. */

#define PAINT_TB_H   28                 /* toolbar height, above the bitmap */
#define PAINT_COLORS 12

enum { PT_PEN = 0, PT_ERASER = 1 };

struct paint {
    int w, h;                           /* the bitmap */
    uint32_t *px;
    uint32_t color;
    int size;                           /* brush radius: 1, 3 or 6 */
    int tool;
    int drawing;                        /* a stroke is in progress */
    int lx, ly;                         /* its last point */
    int strokes;                        /* strokes drawn since the last clear */
    char msg[128];                      /* "saved to ..." and the like, shown in the title bar */
    char last[512];                     /* the path of the last saved picture */
};

enum { PAINT_NONE = 0, PAINT_SHOW = 1 };  /* what paint_press asks the desktop for */

int      paint_init(struct paint *p, int w, int h);
void     paint_free(struct paint *p);
int      paint_width(const struct paint *p);             /* the app's size including the toolbar */
int      paint_height(const struct paint *p);
void     paint_draw(const struct paint *p, struct canvas *c, const struct font *f, int ox, int oy);
/* Mouse, in the app's own coordinates (0,0 = top left of the toolbar).
 * Returns PAINT_SHOW when 「見せる」 was pressed and the picture is saved
 * (its path in p->last): the desktop then shows it to AlterEgo. */
int      paint_press(struct paint *p, const struct font *f, int x, int y, const char *save_dir);
void     paint_move(struct paint *p, int x, int y);
void     paint_release(struct paint *p);
void     paint_clear(struct paint *p);
/* Write the bitmap as PPM into `dir` (created if needed); the path lands in p->msg. */
int      paint_save(struct paint *p, const char *dir);
uint32_t paint_pixel(const struct paint *p, int x, int y);
/* Draw what AlterEgo asks for: a spec of space-separated commands with
 * coordinates in 1/1000 of the sheet's height from the top left (the
 * agent assumes a sheet about 1600 wide): k RRGGBB (color), s R (brush
 * radius), l x1 y1 x2 y2, c cx cy r (circle), C cx cy r (filled),
 * p N x1 y1 … xN yN (polyline), P N … (filled polygon), i path x0 y0 x1 y1
 * (paste the ink of that PPM into the box), e (empty the sheet first).
 * Returns the number of commands done. */
int      paint_run(struct paint *p, const char *spec);
extern const uint32_t paint_palette[PAINT_COLORS];

#endif

#ifndef ALTEREGO_DRAWINGS_H
#define ALTEREGO_DRAWINGS_H

#include <stddef.h>
#include "brain.h"

/* What AlterEgo can draw, and how that grows in conversation.
 *
 * A drawing is a spec for the desktop's paint_run (see paint.h): commands
 * with coordinates in 1/1000 of the sheet's height, the sheet about 1600
 * wide, things centered near (800, 500). Fifteen are built in. New ones
 * are learned two ways and kept in drawings.txt in the store:
 *
 *   - from a picture: the user draws in paint, shows it (「見せる」) and
 *     says 「これは雪だるま」. The picture is copied into drawings/ and the
 *     spec is "i <file> x0 y0 x1 y1" (paint pastes the picture's ink into
 *     that box),
 *   - from words: 「雪だるまはこう描く: 大きい丸の上に小さい丸」. Known
 *     drawings (built in or learned) are placed above / below / left of /
 *     right of / inside one another, bigger or smaller, in a color, and
 *     the placed specs are joined into one. */

#define DRAW_NAME 64
#define DRAW_SPEC 3000

struct drawing {
    char ja[DRAW_NAME], en[DRAW_NAME];      /* en is empty for a learned one taught in Japanese, and so on */
    char spec[DRAW_SPEC];
    int learned;
};

/* The drawing named in `line` (built in or learned); -1 when none. */
int  drawings_find(const char *line, const char *lo, struct drawing *out);
/* The n-th one, for 「何か描いて」; 0 when n is past the end. */
int  drawings_nth(int n, struct drawing *out);
/* "丸、線、…、雪だるま" / "circle, line, …". */
void drawings_list(enum brain_lang lang, char *out, size_t cap);

/* Learn `name` from the PPM at `path`: 0, or -1 with the reason in `err`. */
int  drawings_learn_picture(const char *name, const char *path, enum brain_lang lang, char *err, size_t cap);
/* Learn `name` from a description of known drawings: 0, or -1 with the reason in `err`. */
int  drawings_learn_words(const char *name, const char *desc, enum brain_lang lang, char *err, size_t cap);
/* Forget a learned drawing: 1 when one was forgotten. */
int  drawings_forget(const char *name);

#endif

#ifndef ALTEREGO_SHAPES_H
#define ALTEREGO_SHAPES_H

#include "brain.h"

/* Seeing shapes in a picture: the ink (every pixel that is not paper) is
 * split into connected blobs, and each blob is told apart by how it is
 * spread: thin and straight → a line, a ring with an empty middle → a
 * circle, ink along the edges of its box → a square, a flat base under
 * slanted sides → a triangle, two lobes over a point → a heart, a filled
 * box or disk, otherwise a curve or a blob. A big circle with two blobs in its
 * upper half (and something in its lower half) is a face. */

enum { SH_LINE = 0, SH_CURVE, SH_CIRCLE, SH_DISK, SH_SQUARE, SH_BOX, SH_TRIANGLE, SH_HEART, SH_BLOB, SH_FACE, SH_KINDS };

struct shapes {
    int count[SH_KINDS];
    int total;                  /* shapes seen (specks are not counted) */
    int face_eyes, face_mouth;  /* the first face's parts */
};

/* px: RGB bytes, w × h. */
void shapes_find(const unsigned char *px, int w, int h, struct shapes *out);
/* "丸が 1 つと線が 2 本" / "a circle and two lines"; empty when nothing was recognized. */
void shapes_words(const struct shapes *s, enum brain_lang lang, char *out, size_t cap);

#endif

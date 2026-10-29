#ifndef ALTEREGO_APPS_H
#define ALTEREGO_APPS_H

#include <stddef.h>
#include "brain.h"

/* The desktop's apps, from the conversation:
 *
 *   - 「ペイントを開いて」 / "open the browser" / 「https://… を開いて」 /
 *     「〇〇を調べて」 / 「シェルを閉じて」: the agent cannot draw windows
 *     itself, so it asks the desktop by printing an OSC string
 *     ESC ] 7700;open;<app>[;url] ESC \ that the screen's terminal emulator
 *     keeps for the desktop (ALTEREGO_SCREEN=1 says there is one),
 *   - 「絵を見て」 / "look at my drawing": reads the newest picture paint
 *     saved (or the PPM at a given path) and says what it sees: the
 *     colors, how much of the sheet is used, where and how big. */

struct apps_result {
    char action[128];       /* for the action log ("open paint"), empty when nothing was done */
    char note[96];          /* a short memory of it, empty when there is nothing to keep */
};

/* True when `line` is about an app or a picture: `out` gets the reply (with
 * the OSC request embedded when a window is wanted). `lo` is `line` with
 * ASCII lowercased. */
int apps_handle(enum brain_lang lang, const char *you, const char *line, const char *lo,
                char *out, size_t cap, struct apps_result *r);

/* Describe the PPM picture at `path` (for the tests). */
int apps_describe_picture(enum brain_lang lang, const char *path, char *out, size_t cap, char *note, size_t ncap);

#endif

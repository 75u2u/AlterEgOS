#ifndef ALTEREGO_TERM_H
#define ALTEREGO_TERM_H

#include <stddef.h>
#include <stdint.h>

/* A small terminal emulator: a grid of code points fed with what a pty
 * master reads. Enough for the agent, busybox sh and its line editing
 * (CR, LF, BS, TAB, and the common CSI sequences); full-width characters
 * occupy two cells. SGR keeps bold, underline, reverse and the 16 basic
 * foreground colors (what the browser uses for links and headings); the
 * rest of the colors and attributes are parsed and ignored. */

enum { CELL_NORMAL = 0, CELL_WIDE = 1, CELL_TAIL = 2 };
enum { ATTR_BOLD = 1, ATTR_UNDERLINE = 2, ATTR_REVERSE = 4 };

struct cell {
    uint32_t cp;                /* 0 = empty */
    uint8_t kind;
    uint8_t attr;               /* ATTR_* bits */
    uint8_t fg;                 /* 0 = default, 1..16 = ANSI color + 1 (30..37, 90..97) */
};

struct term {
    int cols, rows;
    struct cell *cells;
    int cx, cy;
    int wrap_pending;           /* the last column was just written: wrap before the next glyph */
    int saved_cx, saved_cy;
    int state;                  /* 0 plain, 1 ESC, 2 CSI, 3 OSC/ignore-to-end */
    char csi[32];
    int csi_len;
    unsigned char utf[4];       /* a UTF-8 sequence split across reads */
    int utf_len, utf_need;
    int dirty;
    uint8_t attr, fg;           /* current SGR state for new characters */
};

int  term_init(struct term *t, int cols, int rows);
void term_free(struct term *t);
void term_feed(struct term *t, const char *buf, size_t n);
void term_putc(struct term *t, uint32_t cp);      /* one printable code point */
/* Row `y` as UTF-8 (trailing spaces trimmed), for tests and logs. */
void term_row(const struct term *t, int y, char *out, size_t cap);

#endif

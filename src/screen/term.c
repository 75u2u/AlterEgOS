#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "term.h"
#include "../agent/utf8.h"

static struct cell *at(struct term *t, int x, int y) { return t->cells + (size_t)y * t->cols + x; }

static void clear_cells(struct term *t, int x0, int y0, int x1, int y1)   /* [x0,x1) on rows [y0,y1) */
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) { struct cell *c = at(t, x, y); c->cp = 0; c->kind = CELL_NORMAL; }
    t->dirty = 1;
}

int term_init(struct term *t, int cols, int rows)
{
    memset(t, 0, sizeof *t);
    t->cols = cols;
    t->rows = rows;
    t->cells = calloc((size_t)cols * (size_t)rows, sizeof *t->cells);
    t->dirty = 1;
    return t->cells ? 0 : -1;
}

void term_free(struct term *t)
{
    free(t->cells);
    t->cells = NULL;
}

static void scroll_up(struct term *t)
{
    memmove(t->cells, t->cells + t->cols, (size_t)(t->rows - 1) * t->cols * sizeof *t->cells);
    clear_cells(t, 0, t->rows - 1, t->cols, t->rows);
}

static void newline(struct term *t)
{
    if (t->cy + 1 >= t->rows) scroll_up(t);
    else t->cy++;
    t->dirty = 1;
}

/* Blank a cell, and the other half of a wide character it belongs to. */
static void blank(struct term *t, int x, int y)
{
    struct cell *c = at(t, x, y);
    if (c->kind == CELL_WIDE && x + 1 < t->cols) { struct cell *n = at(t, x + 1, y); n->cp = 0; n->kind = CELL_NORMAL; }
    if (c->kind == CELL_TAIL && x > 0) { struct cell *p = at(t, x - 1, y); p->cp = 0; p->kind = CELL_NORMAL; }
    c->cp = 0;
    c->kind = CELL_NORMAL;
}

void term_putc(struct term *t, uint32_t cp)
{
    int wide = utf8_class(cp) == CT_HIRAGANA || utf8_class(cp) == CT_KATAKANA || utf8_class(cp) == CT_KANJI ||
               (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
               (cp >= 0x2E80 && cp <= 0x2FFF) || (cp >= 0x3100 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
               (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x3FFFD);
    int w = wide ? 2 : 1;
    if (t->wrap_pending || t->cx + w > t->cols) { t->wrap_pending = 0; t->cx = 0; newline(t); }
    blank(t, t->cx, t->cy);
    if (w == 2) blank(t, t->cx + 1, t->cy);
    struct cell *c = at(t, t->cx, t->cy);
    c->cp = cp;
    c->kind = wide ? CELL_WIDE : CELL_NORMAL;
    if (wide) { struct cell *n = at(t, t->cx + 1, t->cy); n->cp = 0; n->kind = CELL_TAIL; }
    t->cx += w;
    if (t->cx >= t->cols) { t->cx = t->cols - 1; t->wrap_pending = 1; }   /* wrap when the next glyph comes */
    t->dirty = 1;
}

static int csi_arg(const struct term *t, int i, int def)
{
    const char *p = t->csi;
    if (*p == '?' || *p == '>') p++;
    for (int k = 0; k < i; k++) {
        p = strchr(p, ';');
        if (!p) return def;
        p++;
    }
    if (*p == '\0' || *p == ';') return def;
    return atoi(p);
}

static void clamp(struct term *t)
{
    t->wrap_pending = 0;
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy < 0) t->cy = 0;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

static void csi_final(struct term *t, char ch)
{
    int n = csi_arg(t, 0, 1);
    if (n < 1 && ch != 'J' && ch != 'K' && ch != 'm' && ch != 'h' && ch != 'l') n = 1;
    switch (ch) {
    case 'A': t->cy -= n; break;
    case 'B': t->cy += n; break;
    case 'C': t->cx += n; break;
    case 'D': t->cx -= n; break;
    case 'G': t->cx = n - 1; break;
    case 'd': t->cy = n - 1; break;
    case 'H': case 'f': t->cy = csi_arg(t, 0, 1) - 1; t->cx = csi_arg(t, 1, 1) - 1; break;
    case 'J': {
        int m = csi_arg(t, 0, 0);
        if (m == 0) { clear_cells(t, t->cx, t->cy, t->cols, t->cy + 1); clear_cells(t, 0, t->cy + 1, t->cols, t->rows); }
        else if (m == 1) { clear_cells(t, 0, 0, t->cols, t->cy); clear_cells(t, 0, t->cy, t->cx + 1, t->cy + 1); }
        else { clear_cells(t, 0, 0, t->cols, t->rows); t->cx = t->cy = 0; }
        break;
    }
    case 'K': {
        int m = csi_arg(t, 0, 0);
        if (m == 0) clear_cells(t, t->cx, t->cy, t->cols, t->cy + 1);
        else if (m == 1) clear_cells(t, 0, t->cy, t->cx + 1, t->cy + 1);
        else clear_cells(t, 0, t->cy, t->cols, t->cy + 1);
        break;
    }
    case 'P': {                                   /* delete n characters, shift the rest left */
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        struct cell *row = at(t, 0, t->cy);
        memmove(row + t->cx, row + t->cx + n, (size_t)(t->cols - t->cx - n) * sizeof *row);
        clear_cells(t, t->cols - n, t->cy, t->cols, t->cy + 1);
        break;
    }
    case '@': {                                   /* insert n blanks */
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        struct cell *row = at(t, 0, t->cy);
        memmove(row + t->cx + n, row + t->cx, (size_t)(t->cols - t->cx - n) * sizeof *row);
        clear_cells(t, t->cx, t->cy, t->cx + n, t->cy + 1);
        break;
    }
    case 'L': for (int i = 0; i < n; i++) {        /* insert lines at the cursor */
        memmove(at(t, 0, t->cy + 1), at(t, 0, t->cy), (size_t)(t->rows - t->cy - 1) * t->cols * sizeof(struct cell));
        clear_cells(t, 0, t->cy, t->cols, t->cy + 1);
    } break;
    case 'M': for (int i = 0; i < n; i++) {        /* delete lines at the cursor */
        memmove(at(t, 0, t->cy), at(t, 0, t->cy + 1), (size_t)(t->rows - t->cy - 1) * t->cols * sizeof(struct cell));
        clear_cells(t, 0, t->rows - 1, t->cols, t->rows);
    } break;
    case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
    case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; break;
    default: break;                               /* m, h, l, r, n, c ...: ignored */
    }
    clamp(t);
    t->dirty = 1;
}

static void control(struct term *t, uint32_t cp)
{
    if (cp != 0x1B) t->wrap_pending = 0;
    switch (cp) {
    case '\n': case 0x0B: case 0x0C: newline(t); break;
    case '\r': t->cx = 0; break;
    case '\b':
        if (t->cx > 0) {
            t->cx--;
            if (at(t, t->cx, t->cy)->kind == CELL_TAIL && t->cx > 0) t->cx--;
        }
        break;
    case '\t': t->cx = (t->cx / 8 + 1) * 8; if (t->cx >= t->cols) t->cx = t->cols - 1; break;
    case 0x1B: t->state = 1; break;
    default: break;                               /* BEL, NUL, SO/SI ... */
    }
    t->dirty = 1;
}

static void byte(struct term *t, unsigned char b)
{
    switch (t->state) {
    case 1:                                       /* after ESC */
        if (b == '[') { t->state = 2; t->csi_len = 0; t->csi[0] = '\0'; }
        else if (b == ']' || b == 'P' || b == '^' || b == '_') t->state = 3;
        else if (b == '(' || b == ')' || b == '#') t->state = 4;
        else if (b == '7') { t->saved_cx = t->cx; t->saved_cy = t->cy; t->state = 0; }
        else if (b == '8') { t->cx = t->saved_cx; t->cy = t->saved_cy; t->state = 0; }
        else if (b == 'M') { if (t->cy > 0) t->cy--; t->state = 0; }
        else if (b == 'D') { newline(t); t->state = 0; }
        else if (b == 'E') { t->cx = 0; newline(t); t->state = 0; }
        else t->state = 0;
        return;
    case 2:                                       /* CSI parameters */
        if (b >= 0x40 && b <= 0x7E) { t->csi[t->csi_len] = '\0'; csi_final(t, (char)b); t->state = 0; }
        else if (t->csi_len < (int)sizeof t->csi - 1) t->csi[t->csi_len++] = (char)b;
        return;
    case 3:                                       /* OSC etc.: skip to BEL or ST */
        if (b == 0x07) t->state = 0;
        else if (b == 0x1B) t->state = 5;
        return;
    case 4: t->state = 0; return;                 /* charset designation: one byte */
    case 5: t->state = (b == '\\') ? 0 : 3; return;
    default: break;
    }
    if (b < 0x20 || b == 0x7F) { t->utf_len = 0; control(t, b); return; }
    if (b < 0x80) { t->utf_len = 0; term_putc(t, b); return; }
    if ((b & 0xC0) == 0x80) {                     /* continuation */
        if (t->utf_len == 0) return;
        t->utf[t->utf_len++] = b;
        if (t->utf_len == t->utf_need) {
            t->utf[t->utf_len] = '\0';
            uint32_t cp;
            if (utf8_decode((const char *)t->utf, &cp) > 0 && cp != 0xFFFD) term_putc(t, cp);
            else term_putc(t, 0xFFFD);
            t->utf_len = 0;
        }
        return;
    }
    t->utf[0] = b;
    t->utf_len = 1;
    t->utf_need = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
    if (t->utf_need == 0) t->utf_len = 0;
}

void term_feed(struct term *t, const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) byte(t, (unsigned char)buf[i]);
}

void term_row(const struct term *t, int y, char *out, size_t cap)
{
    size_t o = 0;
    int last = 0;
    for (int x = 0; x < t->cols; x++) {
        const struct cell *c = t->cells + (size_t)y * t->cols + x;
        if (c->kind == CELL_TAIL) continue;
        char u[4];
        size_t m = c->cp ? utf8_encode(c->cp, u) : (size_t)(u[0] = ' ', 1);
        if (o + m + 1 > cap) break;
        memcpy(out + o, u, m);
        o += m;
        if (c->cp && c->cp != ' ') last = (int)o;
    }
    out[last] = '\0';
}

/* Shapes in a picture. See shapes.h. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shapes.h"

#define MAX_BLOBS 64

struct blob {
    int n;                      /* ink cells */
    int x0, y0, x1, y1;         /* box */
    double cx, cy;              /* centroid */
    int kind;
    int used;                   /* part of a face */
};

static int paper(const unsigned char *p) { return p[0] > 235 && p[1] > 235 && p[2] > 235; }

/* Flood fill (8-connected) from (sx, sy) in the grid, collecting cells. */
static int fill(unsigned char *g, int gw, int gh, int sx, int sy, int *cells, int cap)
{
    int n = 0, top = 0;
    int *stack = malloc(sizeof(int) * 2 * (size_t)gw * gh);
    if (!stack) return 0;
    stack[top++] = sx; stack[top++] = sy;
    g[sy * gw + sx] = 2;
    while (top > 0) {
        int y = stack[--top], x = stack[--top];
        if (n < cap) { cells[2 * n] = x; cells[2 * n + 1] = y; }
        n++;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || g[ny * gw + nx] != 1) continue;
            g[ny * gw + nx] = 2;
            stack[top++] = nx; stack[top++] = ny;
        }
    }
    free(stack);
    return n;
}

static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static int classify(const int *cells, int n, struct blob *b)
{
    int bw = b->x1 - b->x0 + 1, bh = b->y1 - b->y0 + 1;
    if (bw <= 3 && bh <= 3) return SH_BLOB;                        /* a speck or a dot */
    /* spread: principal axes from the covariance */
    double sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        double dx = cells[2 * i] - b->cx, dy = cells[2 * i + 1] - b->cy;
        sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
    }
    sxx /= n; syy /= n; sxy /= n;
    double tr = sxx + syy, det = sxx * syy - sxy * sxy;
    double disc = sqrt(fmax(tr * tr / 4 - det, 0));
    double l1 = tr / 2 + disc, l2 = fmax(tr / 2 - disc, 0);
    double elong = l1 > 0 ? sqrt(l2 / l1) : 1;
    double aspect = (double)(bw < bh ? bw : bh) / (bw < bh ? bh : bw);
    double fillr = (double)n / ((double)bw * bh);
    if (elong < 0.22) return SH_LINE;
    /* radial spread around the centroid, and whether the middle is empty */
    double *r = malloc(sizeof(double) * (size_t)n);
    if (!r) return SH_BLOB;
    double mean = 0;
    for (int i = 0; i < n; i++) { double dx = cells[2 * i] - b->cx, dy = cells[2 * i + 1] - b->cy; r[i] = sqrt(dx * dx + dy * dy); mean += r[i]; }
    mean /= n;
    double var = 0;
    for (int i = 0; i < n; i++) var += (r[i] - mean) * (r[i] - mean);
    double sd = sqrt(var / n);
    qsort(r, (size_t)n, sizeof *r, cmp_double);
    double r10 = r[n / 10], r90 = r[n * 9 / 10];
    free(r);
    double inner = 0.45 * mean, incount = 0;
    for (int i = 0; i < n; i++) { double dx = cells[2 * i] - b->cx, dy = cells[2 * i + 1] - b->cy; if (dx * dx + dy * dy < inner * inner) incount++; }
    double cdens = incount / (3.14159 * inner * inner + 1);
    /* ink near the edges of the box */
    int band = (int)(0.1 * (bw < bh ? bw : bh)); if (band < 2) band = 2;
    int edge = 0;
    for (int i = 0; i < n; i++) {
        int x = cells[2 * i], y = cells[2 * i + 1];
        if (x - b->x0 < band || b->x1 - x < band || y - b->y0 < band || b->y1 - y < band) edge++;
    }
    double edgef = (double)edge / n;
    /* bands: the top and bottom tenth, the notch (middle tenth of the top quarter), the corners */
    int vb = bh / 10 + 1, top = 0, bottom = 0, notch = b->y1, corners = 0, bx0 = bw, bx1 = -1;
    int cw = bw * 15 / 100 + 1, ch = bh * 15 / 100 + 1;
    for (int i = 0; i < n; i++) {
        int x = cells[2 * i], y = cells[2 * i + 1];
        if (y - b->y0 < vb) top++;
        if (b->y1 - y < vb) { bottom++; if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; }
        if (abs(2 * x - (b->x0 + b->x1)) <= bw / 10 && y < notch) notch = y;     /* the highest ink in the middle column */
        if ((x - b->x0 < cw || b->x1 - x < cw) && (y - b->y0 < ch || b->y1 - y < ch)) corners++;
    }
    double cornerd = (double)corners / (4.0 * cw * ch);
    int hollow = cdens < 0.2;
    if (hollow) {
        if (edgef > 0.85 && aspect > 0.4) return SH_SQUARE;
        int roundish = sd / mean < 0.28 && aspect > 0.55;
        if (notch - b->y0 > bh / 12 && bx1 - bx0 < bw / 4 && aspect > 0.55) return SH_HEART;   /* a notch between two lobes, a point below */
        if (roundish && r90 / (r10 + 0.001) < 1.45) return SH_CIRCLE;
        if ((bottom >= n / 5 && bx1 - bx0 > bw * 6 / 10) || (top >= n / 5)) return SH_TRIANGLE;
        if (roundish && r90 / (r10 + 0.001) < 1.9) return SH_CIRCLE;
        return SH_CURVE;
    }
    if (fillr > 0.85 && aspect > 0.3) return SH_BOX;
    if (fillr > 0.55 && aspect > 0.6 && sd / mean < 0.5 && cornerd < 0.25) return SH_DISK;
    return SH_BLOB;
}

void shapes_find(const unsigned char *px, int w, int h, struct shapes *out)
{
    memset(out, 0, sizeof *out);
    int s = w / 320; if (s < 1) s = 1;
    int gw = w / s, gh = h / s;
    if (gw < 4 || gh < 4) return;
    unsigned char *g = calloc((size_t)gw * gh, 1);
    if (!g) return;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = px + (size_t)y * w * 3;
        int gy = y / s; if (gy >= gh) break;
        for (int x = 0; x < w; x++) { int gx = x / s; if (gx < gw && !paper(row + x * 3)) g[gy * gw + gx] = 1; }
    }
    int cap = gw * gh;
    int *cells = malloc(sizeof(int) * 2 * (size_t)cap);
    struct blob blobs[MAX_BLOBS];
    int nb = 0;
    if (!cells) { free(g); return; }
    for (int y = 0; y < gh && nb < MAX_BLOBS; y++) for (int x = 0; x < gw && nb < MAX_BLOBS; x++) {
        if (g[y * gw + x] != 1) continue;
        int n = fill(g, gw, gh, x, y, cells, cap);
        if (n < 6) continue;                                           /* a speck */
        struct blob *b = &blobs[nb];
        memset(b, 0, sizeof *b);
        b->n = n; b->x0 = b->x1 = cells[0]; b->y0 = b->y1 = cells[1];
        double sx = 0, sy = 0;
        for (int i = 0; i < n; i++) {
            int cx = cells[2 * i], cy = cells[2 * i + 1];
            if (cx < b->x0) b->x0 = cx;
            if (cx > b->x1) b->x1 = cx;
            if (cy < b->y0) b->y0 = cy;
            if (cy > b->y1) b->y1 = cy;
            sx += cx; sy += cy;
        }
        b->cx = sx / n; b->cy = sy / n;
        b->kind = classify(cells, n, b);
        nb++;
    }
    free(cells);
    free(g);
    /* faces: a big circle with things inside it */
    for (int i = 0; i < nb; i++) {
        struct blob *f = &blobs[i];
        if (f->kind != SH_CIRCLE || f->used || (f->x1 - f->x0) < gw / 8) continue;
        int eyes = 0, mouth = 0, parts[MAX_BLOBS], np = 0;
        for (int j = 0; j < nb; j++) {
            struct blob *p = &blobs[j];
            if (j == i || p->used) continue;
            if (p->cx < f->x0 || p->cx > f->x1 || p->cy < f->y0 || p->cy > f->y1) continue;
            if ((p->x1 - p->x0) > (f->x1 - f->x0) * 0.8) continue;      /* as wide as the face: not a part */
            if (p->cy < f->y0 + (f->y1 - f->y0) * 0.55) eyes++; else mouth++;
            parts[np++] = j;
        }
        if (eyes >= 2) {
            f->kind = SH_FACE;
            for (int k = 0; k < np; k++) blobs[parts[k]].used = 1;
            if (!out->count[SH_FACE]) { out->face_eyes = eyes; out->face_mouth = mouth; }
        }
    }
    for (int i = 0; i < nb; i++) {
        if (blobs[i].used) continue;
        if (blobs[i].kind == SH_BLOB && (blobs[i].x1 - blobs[i].x0) <= 3 && (blobs[i].y1 - blobs[i].y0) <= 3) continue;   /* dots are not shapes */
        out->count[blobs[i].kind]++;
        out->total++;
    }
}

void shapes_words(const struct shapes *s, enum brain_lang lang, char *out, size_t cap)
{
    static const char *const ja[SH_KINDS] = { "線", "曲がった線", "丸", "塗った丸", "四角", "塗った四角", "三角", "ハート", "かたまり", "顔" };
    static const char *const ja_unit[SH_KINDS] = { "本", "本", "つ", "つ", "つ", "つ", "つ", "つ", "つ", "つ" };
    static const char *const en1[SH_KINDS] = { "a line", "a curve", "a circle", "a filled circle", "a square", "a filled box", "a triangle", "a heart", "a blob", "a face" };
    static const char *const enN[SH_KINDS] = { "lines", "curves", "circles", "filled circles", "squares", "filled boxes", "triangles", "hearts", "blobs", "faces" };
    static const int order[SH_KINDS] = { SH_FACE, SH_HEART, SH_CIRCLE, SH_DISK, SH_SQUARE, SH_BOX, SH_TRIANGLE, SH_LINE, SH_CURVE, SH_BLOB };
    size_t o = 0;
    int kinds = 0, written = 0;
    out[0] = '\0';
    for (int i = 0; i < SH_KINDS; i++) if (s->count[i]) kinds++;
    if (!kinds) return;
    for (int k = 0; k < SH_KINDS && o + 8 < cap; k++) {
        int i = order[k];
        if (!s->count[i]) continue;
        written++;
        if (lang == BRAIN_EN) {
            if (written > 1) o += (size_t)snprintf(out + o, cap - o, written == kinds ? " and " : ", ");
            if (s->count[i] == 1) o += (size_t)snprintf(out + o, cap - o, "%s", en1[i]);
            else o += (size_t)snprintf(out + o, cap - o, "%d %s", s->count[i], enN[i]);
        } else {
            if (written > 1) o += (size_t)snprintf(out + o, cap - o, written == kinds ? "と" : "、");
            o += (size_t)snprintf(out + o, cap - o, "%sが %d %s", ja[i], s->count[i], ja_unit[i]);
        }
    }
}

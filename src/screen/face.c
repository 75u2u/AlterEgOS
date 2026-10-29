/* AlterEgo's face (see face.h): drawn from circles, ellipses and arcs on
 * the canvas, no image files. Everything is a function of the agent's
 * state and the time, so it is as alive as the agent is. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "face.h"

#define EXPR_MS    4000         /* how long a told expression holds */
#define LISTEN_MS  1800         /* after a key: looking up, listening */
#define DROWSY_MS  90000L       /* silence before the eyes droop */
#define SLEEP_MS   180000L      /* ... and close */
#define SAY_MS_PER_CP 70
#define SAY_MIN_MS 500

#define C_SKIN   0xFFE9DA
#define C_SHADE  0xF2C9B4
#define C_LINE   0x5A3E4A         /* the ink of the line art */
#define C_HAIR   0xB9A5DC         /* soft lavender */
#define C_HAIR_D 0x8E78B8         /* its shadow */
#define C_HAIR_L 0xE2D8F4         /* its shine */
#define C_WHITE  0xFFFFFF
#define C_IRIS   0x4FB0CC         /* teal */
#define C_IRIS_D 0x2A6E8C
#define C_IRIS_L 0x9FE3F0
#define C_INK    0x2B1D26
#define C_MOUTH  0xD8707A
#define C_TONGUE 0xF0A0A8
#define C_BLUSH  0xFFB6C1
#define C_SWEAT  0x7FC8FF
#define C_HEART  0xFF6B8A
#define C_TEXT   0xDDE3EA
#define C_DIM    0x7F8894
#define C_Z      0xA0B0C0
#define C_BG     0x1A2230

static const uint32_t c_cloth[4] = { 0xFFF1DC, 0xFFE6A8, 0xA8E6D4, 0x9ACFF0 };
static const uint32_t c_cloth_d[4] = { 0xE8D4B8, 0xE8C880, 0x78C0A8, 0x68A8D8 };

/* ---- drawing primitives, clipped to the canvas */

struct clip { int x0, y0, x1, y1; };
static struct clip g_clip;

static void put(struct canvas *cv, int x, int y, uint32_t c)
{
    if (x < g_clip.x0 || y < g_clip.y0 || x >= g_clip.x1 || y >= g_clip.y1) return;
    cv->px[(size_t)y * cv->w + x] = c;
}

static void ellipse(struct canvas *cv, int cx, int cy, int rx, int ry, uint32_t c)
{
    if (rx <= 0 || ry <= 0) return;
    for (int y = -ry; y <= ry; y++) {
        double f = 1.0 - (double)y * y / ((double)ry * ry);
        if (f < 0) continue;
        int hw = (int)(rx * sqrt(f) + 0.5);
        for (int x = -hw; x <= hw; x++) put(cv, cx + x, cy + y, c);
    }
}

static void disc(struct canvas *cv, int cx, int cy, int r, uint32_t c) { ellipse(cv, cx, cy, r, r, c); }

static void line(struct canvas *cv, int x0, int y0, int x1, int y1, int thick, uint32_t c)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    if (n == 0) { disc(cv, x0, y0, thick / 2, c); return; }
    for (int i = 0; i <= n; i++) {
        int x = x0 + (x1 - x0) * i / n, y = y0 + (y1 - y0) * i / n;
        if (thick <= 1) put(cv, x, y, c); else disc(cv, x, y, thick / 2, c);
    }
}

/* An arc of the ellipse (cx,cy,rx,ry) from angle a0 to a1 (degrees, 0 = right, 90 = down). */
static void arc(struct canvas *cv, int cx, int cy, int rx, int ry, double a0, double a1, int thick, uint32_t c)
{
    int steps = (int)(fabs(a1 - a0) / 4) + 2;
    int px = 0, py = 0;
    for (int i = 0; i <= steps; i++) {
        double a = (a0 + (a1 - a0) * i / steps) * M_PI / 180.0;
        int x = cx + (int)lround(rx * cos(a)), y = cy + (int)lround(ry * sin(a));
        if (i) line(cv, px, py, x, y, thick, c);
        px = x; py = y;
    }
}

/* A curve from (x0,y) to (x1,y) bending `bend` pixels (down when positive). */
static void bow(struct canvas *cv, int x0, int x1, int y, int bend, int thick, uint32_t c)
{
    int n = 12, px = x0, py = y;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n;
        int x = x0 + (int)lround((x1 - x0) * t), yy = y + (int)lround(bend * 4 * t * (1 - t));
        line(cv, px, py, x, yy, thick, c);
        px = x; py = yy;
    }
}

/* A filled triangle (scanline). */
static void tri(struct canvas *cv, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c)
{
    int ymin = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int ymax = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = ymin; y <= ymax; y++) {
        int xs[3], n = 0;
        int px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
        for (int e = 0; e < 3; e++) {
            int ax = px[e], ay = py[e], bx = px[(e + 1) % 3], by = py[(e + 1) % 3];
            if (ay == by) continue;
            if ((y >= ay && y <= by) || (y >= by && y <= ay)) xs[n++] = ax + (int)lround((double)(bx - ax) * (y - ay) / (by - ay));
        }
        if (n < 2) continue;
        int lo = xs[0], hi = xs[0];
        for (int m = 1; m < n; m++) { if (xs[m] < lo) lo = xs[m]; if (xs[m] > hi) hi = xs[m]; }
        for (int x = lo; x <= hi; x++) put(cv, x, y, c);
    }
}

/* A spike of hair: a tapered triangle with its line-art edge. */
static void spike(struct canvas *cv, int bx0, int bx1, int by, int tx, int ty, uint32_t fill)
{
    tri(cv, bx0 - 1, by, bx1 + 1, by, tx, ty + 2, C_LINE);
    tri(cv, bx0, by, bx1, by, tx, ty, fill);
}

/* ---- state */

static uint32_t rnd(struct face *f) { f->rng ^= f->rng << 13; f->rng ^= f->rng >> 17; f->rng ^= f->rng << 5; return f->rng; }

void face_init(struct face *f, long now)
{
    memset(f, 0, sizeof *f);
    f->rng = 2463534242u;
    f->last_heard = now;
    f->last_key = now - LISTEN_MS;              /* not listening yet */
    f->blink_next = now + 2000;
    snprintf(f->label, sizeof f->label, "ふつう");
}

void face_set(struct face *f, const char *kv, long now)
{
    int say = 0;
    f->expr[0] = '\0';
    while (*kv) {
        size_t n = strcspn(kv, ";");
        char key[16], val[32];
        const char *eq = memchr(kv, '=', n);
        if (eq) {
            snprintf(key, sizeof key, "%.*s", (int)(eq - kv), kv);
            snprintf(val, sizeof val, "%.*s", (int)(n - (size_t)(eq - kv) - 1), eq + 1);
            if (strcmp(key, "mood") == 0) f->mood = atoi(val);
            else if (strcmp(key, "bond") == 0) f->bond = atoi(val);
            else if (strcmp(key, "stage") == 0) f->stage = atoi(val);
            else if (strcmp(key, "body") == 0) f->body = atoi(val);
            else if (strcmp(key, "say") == 0) say = atoi(val);
            else if (strcmp(key, "expr") == 0) snprintf(f->expr, sizeof f->expr, "%.15s", val);
        }
        kv += n;
        if (*kv == ';') kv++;
    }
    f->mood = f->mood < -100 ? -100 : f->mood > 100 ? 100 : f->mood;
    f->bond = f->bond < 0 ? 0 : f->bond > 100 ? 100 : f->bond;
    f->stage = f->stage < 0 ? 0 : f->stage > 3 ? 3 : f->stage;
    f->body = f->body < 0 ? 0 : f->body > 3 ? 3 : f->body;
    f->expr_until = f->expr[0] ? now + EXPR_MS : 0;
    f->last_heard = now;
    if (say > 0) {
        long ms = (long)say * SAY_MS_PER_CP;
        if (ms < SAY_MIN_MS) ms = SAY_MIN_MS;
        if (ms > 6000) ms = 6000;
        f->say_until = now + ms;
    }
}

void face_typing(struct face *f, long now) { f->last_key = now; }

void face_look(struct face *f, int dx, int dy)
{
    f->target_x = dx < -80 ? -4 : dx > 80 ? 4 : dx / 20;
    f->target_y = dy < -60 ? -3 : dy > 60 ? 3 : dy / 20;
}

int face_talking(const struct face *f, long now) { return now < f->say_until; }

const char *face_expr(const struct face *f, long now)
{
    if (f->expr[0] && now < f->expr_until) return f->expr;
    if (f->body >= 3) return "dizzy";
    if (f->body == 2) return "tired";
    long quiet = now - (f->last_key > f->last_heard ? f->last_key : f->last_heard);
    if (quiet >= SLEEP_MS) return "sleep";
    if (quiet >= DROWSY_MS) return "drowsy";
    if (now - f->last_key < LISTEN_MS) return "listen";
    if (f->mood > 30) return "happy";
    if (f->mood < -30) return "sad";
    return "calm";
}

int face_tick(struct face *f, long now)
{
    if (now >= f->blink_next) { f->blink_at = now; f->blink_next = now + 2500 + (long)(rnd(f) % 3500); }
    if (f->look_x != f->target_x) f->look_x += f->look_x < f->target_x ? 1 : -1;
    if (f->look_y != f->target_y) f->look_y += f->look_y < f->target_y ? 1 : -1;
    return 1;                   /* it breathes: every frame moves a little */
}

/* ---- drawing */

static const char *label_of(const char *expr)
{
    static const struct { const char *e, *ja; } t[] = {
        { "happy", "うれしい" }, { "sad", "しょんぼり" }, { "shy", "てれてる" }, { "curious", "きになる" },
        { "puzzled", "？" }, { "surprised", "びっくり" }, { "nod", "おぼえた" }, { "love", "だいすき" },
        { "angry", "むっ" }, { "tired", "つかれた" }, { "dizzy", "くらくら" }, { "sleep", "ねている" },
        { "drowsy", "ねむい" }, { "listen", "きいてる" }, { "calm", "ふつう" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strcmp(t[i].e, expr) == 0) return t[i].ja;
    return expr;
}

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

void face_draw(struct face *f, struct canvas *cv, const struct font *font, int x, int y, long now)
{
    g_clip = (struct clip){ x < 0 ? 0 : x, y < 0 ? 0 : y, x + FACE_W > cv->w ? cv->w : x + FACE_W, y + FACE_H > cv->h ? cv->h : y + FACE_H };
    const char *e = face_expr(f, now);
    int talking = face_talking(f, now);
    int asleep = eq(e, "sleep");
    double t = now / 1000.0;
    /* breathing, a nod while talking, a shiver when dizzy */
    int bob = (int)lround(2.0 * sin(t * 2 * M_PI / 2.4));
    if (talking) bob += (int)lround(1.5 * sin(t * 2 * M_PI / 0.3));
    if (eq(e, "dizzy")) bob += (int)lround(2.0 * sin(t * 2 * M_PI / 0.15));
    int tilt = eq(e, "curious") || eq(e, "puzzled") ? 6 : 0;         /* the head leans */
    int R = 56 + 2 * f->stage;                                        /* the head: wide at the cheeks, a soft chin */
    int cx = x + FACE_W / 2 + tilt, cy = y + 104 + bob;
    int st = f->stage;

    /* the body: narrow shoulders, a round collar, line art around it */
    int bw = 36 + 4 * st, by = cy + R - 6;
    int bottom = y + FACE_H - 32;
    ellipse(cv, x + FACE_W / 2, by + 24, bw + 2, 22, C_LINE);
    canvas_fill(cv, x + FACE_W / 2 - bw - 2, by + 24, 2 * bw + 5, bottom - (by + 24), C_LINE);
    ellipse(cv, x + FACE_W / 2, by + 24, bw, 20, c_cloth[st]);
    canvas_fill(cv, x + FACE_W / 2 - bw, by + 24, 2 * bw + 1, bottom - (by + 24), c_cloth[st]);
    canvas_fill(cv, x + FACE_W / 2 - bw, by + 30, 2 * bw + 1, 2, c_cloth_d[st]);        /* a seam */
    disc(cv, x + FACE_W / 2, by + 2, 9, C_SHADE);                                      /* the neck */
    ellipse(cv, x + FACE_W / 2, by + 10, 13, 5, C_LINE);
    ellipse(cv, x + FACE_W / 2, by + 10, 11, 3, C_SKIN);                                /* the collar opening */

    /* hair behind the head: a soft mass that sits high and stops at the cheeks, fuller with age */
    if (st >= 1) {
        struct clip saved = g_clip;
        int hr = R + 4 + 3 * st;
        if (g_clip.y1 > cy + R / 4) g_clip.y1 = cy + R / 4;              /* nothing below cheek level */
        ellipse(cv, cx, cy - 14, hr + 2, hr - 2, C_LINE);
        ellipse(cv, cx, cy - 14, hr, hr - 4, C_HAIR_D);
        g_clip = saved;
        if (st >= 2) {                                                /* side locks down the cheeks */
            spike(cv, cx - R - 4, cx - R + 14, cy - 10, cx - R - 6, cy + R + 8 + 6 * (st - 2), C_HAIR_D);
            spike(cv, cx + R - 14, cx + R + 4, cy - 10, cx + R + 6, cy + R + 8 + 6 * (st - 2), C_HAIR_D);
        }
    }
    /* the face: round at the cheeks, a small soft chin; line art first */
    int chin_y = cy + (int)(R * 0.42), chin_rx = (int)(R * 0.74), chin_ry = (int)(R * 0.56);
    ellipse(cv, cx, cy - 4, R + 2, R + 1, C_LINE);
    ellipse(cv, cx, chin_y, chin_rx + 2, chin_ry + 2, C_LINE);
    ellipse(cv, cx, cy - 4, R, R - 1, C_SKIN);
    ellipse(cv, cx, chin_y, chin_rx, chin_ry, C_SKIN);

    /* the fringe: pointed bangs across the forehead, a shine, an ahoge */
    int hl = cy - R + 6;                                              /* the hairline */
    if (st == 0) {
        spike(cv, cx - 12, cx + 2, hl - 2, cx - 10, hl + 16, C_HAIR);
        spike(cv, cx - 2, cx + 12, hl - 2, cx + 10, hl + 14, C_HAIR);
        ellipse(cv, cx, hl - 4, 14, 6, C_LINE);
        ellipse(cv, cx, hl - 4, 12, 4, C_HAIR);
    } else {
        /* the cap of hair: the top of a bigger ellipse, down to a wavy fringe line */
        int hrx = R + 4, hry = R + 2, hcy = cy - 8;
        for (int i = -hrx; i <= hrx; i++) {
            double fr = 1.0 - (double)i * i / ((double)hrx * hrx);
            if (fr < 0) continue;
            int top = hcy - (int)(hry * sqrt(fr) + 0.5);
            int fringe = hl + 12 + (int)lround(4 * cos(i * 0.25));
            if (abs(i) > R - 6) fringe = cy + R / 3;                 /* down the sides, over the ears */
            for (int yy = top - 2; yy < fringe + 2; yy++) put(cv, cx + i, yy, C_LINE);
        }
        for (int i = -hrx + 2; i <= hrx - 2; i++) {
            double fr = 1.0 - (double)i * i / ((double)(hrx - 2) * (hrx - 2));
            if (fr < 0) continue;
            int top = hcy - (int)((hry - 2) * sqrt(fr) + 0.5);
            int fringe = hl + 12 + (int)lround(4 * cos(i * 0.25));
            if (abs(i) > R - 6) fringe = cy + R / 3;
            for (int yy = top; yy < fringe; yy++) put(cv, cx + i, yy, C_HAIR);
        }
        /* the bangs: sharp spikes hanging over the forehead, the middle ones longer */
        int nb = 6 + st;
        for (int k = 0; k < nb; k++) {
            double u = (k + 0.5) / nb;                                /* 0..1 across the forehead */
            int bx0 = cx - R + (int)lround(2.0 * R * (k / (double)nb)) + 2;
            int bx1 = cx - R + (int)lround(2.0 * R * ((k + 1) / (double)nb)) - 2;
            double curve = 1.0 - 4.0 * (u - 0.5) * (u - 0.5);           /* longer in the middle */
            int len = 14 + (int)lround(14 * curve) + ((k % 2) ? 6 : 0);
            int tx = (bx0 + bx1) / 2 + (k < nb / 2 ? -3 : 3);
            spike(cv, bx0, bx1, hl + 8, tx, hl + 8 + len, C_HAIR);
        }
        /* a shine band across the hair */
        arc(cv, cx, hcy, R - 8, R - 12, 220, 320, 3, C_HAIR_L);
    }
    {   /* the ahoge: one strand standing up, swaying */
        int sway = (int)lround(3 * sin(t * 2 * M_PI / 1.7));
        int ax = cx + 8, ay = cy - R - (st ? 4 : 0);
        line(cv, ax, ay, ax + 8 + sway, ay - 14, 5, C_LINE);
        line(cv, ax + 8 + sway, ay - 14, ax + 2 + sway, ay - 24, 5, C_LINE);
        line(cv, ax, ay, ax + 8 + sway, ay - 14, 3, C_HAIR);
        line(cv, ax + 8 + sway, ay - 14, ax + 2 + sway, ay - 24, 3, C_HAIR);
    }
    if (st >= 3) {                                                    /* a small star clip in the hair */
        int sx0 = cx - R + 16, sy0 = cy - R + 30;
        for (int k = 0; k < 5; k++) {
            double a0 = (-90 + k * 72) * M_PI / 180.0, a1 = (-90 + (k + 2) * 72) * M_PI / 180.0;
            line(cv, sx0 + (int)lround(7 * cos(a0)), sy0 + (int)lround(7 * sin(a0)), sx0 + (int)lround(7 * cos(a1)), sy0 + (int)lround(7 * sin(a1)), 3, 0xFFD866);
        }
        disc(cv, sx0, sy0, 3, 0xFFD866);
    }

    /* cheeks: a pink flush with three little strokes, stronger when shy or loved */
    int rosy = f->bond >= 35 || eq(e, "shy") || eq(e, "love") || eq(e, "happy");
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        int bx = cx + sgn * (R / 2 + 2), byy = cy + R / 3 - 2;
        ellipse(cv, bx, byy, rosy ? 12 : 9, rosy ? 6 : 4, rosy ? C_BLUSH : 0xFFD5D0);
        if (rosy) for (int k = -1; k <= 1; k++) line(cv, bx + k * 6 - 2, byy - 3, bx + k * 6 + 2, byy + 3, 1, 0xE88898);
    }

    /* the eyes: large, far apart, set low; an outline, a thick upper lid
     * with a lash, a coloured iris darker at the top, two lights */
    int ex = R / 2 + 2, ey = cy + R / 8;
    long since_blink = now - f->blink_at;
    double blink = since_blink < 180 ? 1.0 - labs(since_blink - 90) / 90.0 : 0;   /* 0 open .. 1 closed */
    int closed = asleep || eq(e, "nod") || blink > 0.75;
    int droop = eq(e, "drowsy") || eq(e, "tired") ? 1 : 0;
    int wide = eq(e, "surprised") || eq(e, "listen") || eq(e, "curious");
    int rx0 = st == 0 ? 10 : 12, ry0 = st == 0 ? 11 : 15;
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        int ecx = cx + sgn * ex, ecy = ey;
        if (eq(e, "dizzy")) {                                   /* spirals */
            arc(cv, ecx, ecy, 9, 9, 0, 300, 2, C_INK);
            arc(cv, ecx, ecy, 5, 5, 60, 360, 2, C_INK);
            continue;
        }
        if (eq(e, "happy") && st >= 1) {                        /* smiling eyes: ^ ^ */
            arc(cv, ecx, ecy + 5, rx0, ry0 - 4, 200, 340, 4, C_INK);
            line(cv, ecx + sgn * (rx0 + 1), ecy - 2, ecx + sgn * (rx0 + 5), ecy - 6, 3, C_INK);   /* the lash */
            continue;
        }
        if (closed) {
            bow(cv, ecx - rx0, ecx + rx0, ecy + 1, asleep ? 4 : 3, 3, C_INK);
            line(cv, ecx + sgn * (rx0 + 1), ecy + 1, ecx + sgn * (rx0 + 5), ecy - 3, 3, C_INK);
            continue;
        }
        int rx = rx0 + (wide ? 1 : 0), ry = (int)lround((ry0 + (wide ? 3 : 0)) * (1.0 - blink * 0.9)) - droop * 6;
        if (ry < 3) ry = 3;
        ellipse(cv, ecx, ecy, rx + 2, ry + 2, C_LINE);          /* the outline */
        ellipse(cv, ecx, ecy, rx, ry, C_WHITE);
        int ix = ecx + f->look_x, iy = ecy + 2 + f->look_y;
        int irx = rx - 3, iry = ry - 2 > 2 ? ry - 2 : 2;
        uint32_t ci = eq(e, "love") ? C_HEART : C_IRIS, cd = eq(e, "love") ? 0xD04060 : C_IRIS_D, cl = eq(e, "love") ? 0xFFB0C0 : C_IRIS_L;
        ellipse(cv, ix, iy, irx + 1, iry + 1, C_INK);            /* the iris rim */
        ellipse(cv, ix, iy, irx, iry, ci);
        ellipse(cv, ix, iy - iry / 2, irx - 1, iry / 2 + 1, cd); /* darker at the top (the lid's shadow) */
        ellipse(cv, ix, iy + iry / 3, irx / 2, iry / 3 + 1, cl); /* lighter at the bottom */
        ellipse(cv, ix, iy, irx / 2 + 1, iry / 2 + 1, C_INK);    /* the pupil */
        ellipse(cv, ix - irx / 2, iy - iry / 2, 4, 3, C_WHITE);  /* the big light */
        disc(cv, ix + irx / 2, iy + iry / 2, 2, C_WHITE);        /* the small one */
        /* the upper lid: thick, with a lash at the outer corner; the lower lid: thin */
        arc(cv, ecx, ecy, rx + 1, ry + 1, 200, 340, 4, C_INK);
        line(cv, ecx + sgn * (rx + 1), ecy - ry / 2, ecx + sgn * (rx + 6), ecy - ry / 2 - 5, 3, C_INK);
        arc(cv, ecx, ecy, rx, ry, 30, 150, 1, C_LINE);
        if (droop) { ellipse(cv, ecx, ecy - ry, rx + 3, 6, C_SKIN); arc(cv, ecx, ecy - ry + 4, rx + 1, 3, 180, 360, 3, C_INK); }
    }

    /* eyebrows: thin, from the chatty stage on */
    if (st >= 2 && !asleep) {
        int by0 = ey - ry0 - 12;
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int x0 = cx + sgn * (ex - 9), x1 = cx + sgn * (ex + 9);
            int inner = 0, outer = 0;                           /* heights of the inner and outer ends */
            if (eq(e, "sad") || eq(e, "shy")) { inner = -4; outer = 2; }
            else if (eq(e, "angry")) { inner = 4; outer = -3; }
            else if (eq(e, "surprised") || eq(e, "curious") || eq(e, "listen")) { inner = -3; outer = -3; }
            else if (eq(e, "happy") || eq(e, "love")) { inner = -1; outer = -2; }
            line(cv, x0, by0 + inner, x1, by0 + outer, 2, C_HAIR_D);
        }
    }

    /* the nose: a dot; the mouth: small */
    disc(cv, cx + 1, cy + R / 2 - 6, 1, C_SHADE);
    int my = cy + R / 2 + 6;
    if (talking) {
        int open = (int)lround(fabs(sin(t * 2 * M_PI / 0.22)) * 5) + 2;
        ellipse(cv, cx, my + 1, 5, open + 1, C_LINE);
        ellipse(cv, cx, my + 1, 4, open, C_MOUTH);
        if (open > 4) ellipse(cv, cx, my + 1 + open / 2, 2, open / 3, C_TONGUE);
    } else if (eq(e, "happy") || eq(e, "love") || eq(e, "shy")) {
        tri(cv, cx - 6, my - 2, cx + 6, my - 2, cx, my + 5, C_LINE);  /* a little open smile */
        tri(cv, cx - 4, my - 1, cx + 4, my - 1, cx, my + 3, C_MOUTH);
    } else if (eq(e, "sad") || eq(e, "tired") || eq(e, "dizzy")) {
        bow(cv, cx - 5, cx + 5, my + 2, -3, 2, C_LINE);
    } else if (eq(e, "surprised") || eq(e, "puzzled")) {
        ellipse(cv, cx, my + 1, 3, eq(e, "surprised") ? 5 : 3, C_LINE);
        ellipse(cv, cx, my + 1, 2, eq(e, "surprised") ? 4 : 2, C_MOUTH);
    } else if (eq(e, "angry")) {
        line(cv, cx - 6, my + 1, cx + 6, my + 1, 2, C_LINE);
    } else if (asleep || eq(e, "drowsy")) {
        ellipse(cv, cx, my + 1, 2, 2, C_MOUTH);
    } else {
        bow(cv, cx - 4, cx + 4, my, 2, 2, C_LINE);               /* calm, listening, curious, nod */
    }

    /* the small signs above the head */
    int sx = cx + R - 2, sy = cy - R - 12;
    if (eq(e, "puzzled") || eq(e, "curious")) font_text(font, cv, sx, sy - 8, "？", C_TEXT, C_BG);
    else if (eq(e, "surprised")) font_text(font, cv, sx, sy - 8, "！", C_TEXT, C_BG);
    else if (eq(e, "love") || (eq(e, "happy") && f->bond >= 85)) {
        int hy = sy - 4 - (int)lround(3 * sin(t * 2 * M_PI / 1.1));
        disc(cv, sx - 4, hy, 4, C_HEART); disc(cv, sx + 3, hy, 4, C_HEART);
        line(cv, sx - 8, hy + 1, sx, hy + 9, 2, C_HEART); line(cv, sx + 7, hy + 1, sx, hy + 9, 2, C_HEART);
    } else if (eq(e, "tired")) {
        ellipse(cv, cx + R - 6, cy - R / 3, 3, 5, C_SWEAT);
    } else if (asleep) {
        int k = (int)(now / 700) % 3;
        for (int i = 0; i <= k; i++) font_text(font, cv, sx + i * 10, sy - 10 - i * 12, "z", C_Z, C_BG);
    } else if (eq(e, "happy") && f->mood > 60) font_text(font, cv, sx, sy - 8, "♪", C_TEXT, C_BG);

    /* the word under the face */
    snprintf(f->label, sizeof f->label, "%s", talking ? "はなしてる" : label_of(e));
    static const char *const bond_name[] = { "初対面", "顔見知り", "友達", "親友", "家族同然" };
    const char *bn = bond_name[f->bond >= 85 ? 4 : f->bond >= 60 ? 3 : f->bond >= 35 ? 2 : f->bond >= 15 ? 1 : 0];
    char text[64];
    snprintf(text, sizeof text, "%s ・ %s", f->label, bn);
    int tw = font_text_width(font, text);
    font_text(font, cv, x + (FACE_W - tw) / 2, y + FACE_H - 22, text, C_TEXT, C_BG);
}

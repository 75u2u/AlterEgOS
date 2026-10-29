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

#define C_SKIN   0xFFDFC4
#define C_SHADE  0xE8B898
#define C_HAIR   0x6B4F3A
#define C_WHITE  0xFFFFFF
#define C_IRIS   0x3A2A20
#define C_INK    0x2B1D16
#define C_MOUTH  0xC0504D
#define C_TONGUE 0xE88080
#define C_BLUSH  0xFFB3B3
#define C_SWEAT  0x7FC8FF
#define C_HEART  0xFF6B8A
#define C_TEXT   0xDDE3EA
#define C_DIM    0x7F8894
#define C_Z      0xA0B0C0

static const uint32_t c_cloth[4] = { 0xF5E6C8, 0xFFE2A8, 0x9ED9C5, 0x7CC4D9 };

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
    int R = 56 + 3 * f->stage;                                        /* a big round head: chibi */
    int cx = x + FACE_W / 2 + tilt, cy = y + 106 + bob;

    /* the body: small, round shoulders, a round collar, the colour of its age */
    uint32_t cloth = c_cloth[f->stage];
    int bw = 40 + 5 * f->stage, by = cy + R - 10;
    ellipse(cv, x + FACE_W / 2, by + 26, bw, 24, cloth);
    canvas_fill(cv, x + FACE_W / 2 - bw, by + 26, 2 * bw + 1, y + FACE_H - 34 - (by + 26), cloth);
    disc(cv, x + FACE_W / 2, by + 2, 10, C_SHADE);                 /* the neck */
    ellipse(cv, x + FACE_W / 2, by + 12, 14, 5, C_SKIN);            /* the collar opening */

    /* hair behind the head: fuller with age, never long */
    if (f->stage >= 2) ellipse(cv, cx, cy - 14, R + 3 + 2 * (f->stage - 2), R - 4, C_HAIR);
    /* the head: a little wider than tall, with a soft chin */
    ellipse(cv, cx, cy, R, R - 2, C_SKIN);
    /* hair on top: a newborn's tuft; then soft tufts of short hair that
     * cover the top of the head down to a wavy fringe, side tufts later */
    if (f->stage == 0) { disc(cv, cx + 3, cy - R + 3, 7, C_HAIR); disc(cv, cx - 5, cy - R + 7, 5, C_HAIR); }
    else {
        int fringe_base = cy - R / 3 - (f->stage == 1 ? 6 : 0);
        for (int i = -R; i <= R; i++) {
            double fr = 1.0 - (double)i * i / ((double)R * R);
            if (fr < 0) continue;
            int top = cy - (int)((R - 2) * sqrt(fr) + 0.5) - 3;
            int fringe = fringe_base + (int)lround(7 * cos(i * 0.22 + 0.6)) + (int)lround(3 * sin(i * 0.61));
            if (abs(i) > R - 12) fringe = cy + (f->stage >= 2 ? R / 4 : 0);   /* down over the ears */
            for (int yy = top; yy < fringe; yy++) put(cv, cx + i, yy, C_HAIR);
        }
        /* tufts along the top so it looks fluffy, not a helmet */
        for (int k = -3; k <= 3; k++) {
            double a = (k * 22.0 - 90.0) * M_PI / 180.0;
            disc(cv, cx + (int)lround((R - 4) * cos(a)), cy - 2 + (int)lround((R - 6) * sin(a)), 10 + f->stage, C_HAIR);
        }
    }
    /* the ahoge: one strand standing up, swaying */
    {
        int sway = (int)lround(3 * sin(t * 2 * M_PI / 1.7));
        int ax = cx + 6, ay = cy - R + (f->stage == 0 ? 2 : -2);
        line(cv, ax, ay, ax + 6 + sway, ay - 14, 3, C_HAIR);
        line(cv, ax + 6 + sway, ay - 14, ax + 2 + sway, ay - 22, 3, C_HAIR);
    }
    if (f->stage >= 3) {                                                /* a small star clip in the hair */
        int sx0 = cx - R + 22, sy0 = cy - R + 24;
        for (int k = 0; k < 5; k++) {
            double a0 = (-90 + k * 72) * M_PI / 180.0, a1 = (-90 + (k + 2) * 72) * M_PI / 180.0;
            line(cv, sx0 + (int)lround(7 * cos(a0)), sy0 + (int)lround(7 * sin(a0)), sx0 + (int)lround(7 * cos(a1)), sy0 + (int)lround(7 * sin(a1)), 2, 0xFFD866);
        }
        disc(cv, sx0, sy0, 3, 0xFFD866);
    }

    /* cheeks: always a little rosy, more from friendship on and when shy or loved */
    int rosy = f->bond >= 35 || eq(e, "shy") || eq(e, "love") || eq(e, "happy");
    ellipse(cv, cx - R / 2, cy + R / 4, rosy ? 11 : 8, rosy ? 6 : 4, rosy ? C_BLUSH : 0xFFCFC0);
    ellipse(cv, cx + R / 2, cy + R / 4, rosy ? 11 : 8, rosy ? 6 : 4, rosy ? C_BLUSH : 0xFFCFC0);

    /* eyes: big, set low on the face */
    int ex = R / 2 - 3, ey = cy + R / 12;
    long since_blink = now - f->blink_at;
    double blink = since_blink < 180 ? 1.0 - labs(since_blink - 90) / 90.0 : 0;   /* 0 open .. 1 closed */
    int closed = asleep || eq(e, "nod") || blink > 0.75;
    int droop = eq(e, "drowsy") || eq(e, "tired") ? 1 : 0;
    int wide = eq(e, "surprised") || eq(e, "listen") || eq(e, "curious");
    for (int s = -1; s <= 1; s += 2) {
        int ecx = cx + s * ex, ecy = ey;
        if (eq(e, "dizzy")) {                                   /* spirals */
            arc(cv, ecx, ecy, 8, 8, 0, 300, 2, C_INK);
            arc(cv, ecx, ecy, 4, 4, 60, 360, 2, C_INK);
            continue;
        }
        if (eq(e, "happy") && f->stage >= 1) {                  /* smiling eyes: arcs */
            arc(cv, ecx, ecy + 4, 10, 8, 200, 340, 3, C_INK);
            continue;
        }
        if (closed) { bow(cv, ecx - 9, ecx + 9, ecy + 2, 3, 2, C_INK); continue; }
        if (f->stage == 0) {                                    /* a newborn: big dots */
            int r = wide ? 7 : 6;
            disc(cv, ecx + f->look_x / 2, ecy + f->look_y / 2, r, C_INK);
            disc(cv, ecx + f->look_x / 2 - 2, ecy + f->look_y / 2 - 2, 2, C_WHITE);
            continue;
        }
        int rx = 9 + (wide ? 1 : 0), ry = (int)lround((12 + (wide ? 3 : 0)) * (1.0 - blink * 0.9)) - droop * 5;
        if (ry < 2) ry = 2;
        ellipse(cv, ecx, ecy, rx, ry, C_WHITE);
        int ix = ecx + f->look_x, iy = ecy + f->look_y;
        int iry = ry < 8 ? ry : 8;
        ellipse(cv, ix, iy, 6, iry, eq(e, "love") ? C_HEART : C_IRIS);
        ellipse(cv, ix, iy + 2, 4, iry > 4 ? iry - 3 : 1, eq(e, "love") ? 0xFF9BB0 : 0x6B4A3A);   /* the lighter lower iris */
        disc(cv, ix - 2, iy - 3, 2 + (f->bond >= 60), C_WHITE);  /* the big sparkle */
        disc(cv, ix + 3, iy + 3, 1, C_WHITE);                      /* and the small one */
        if (droop) ellipse(cv, ecx, ecy - ry, rx + 1, 5, C_SKIN); /* the lid */
    }

    /* eyebrows, from the chatty stage on: thin and soft */
    if (f->stage >= 2 && !asleep) {
        int by0 = ey - 20;
        for (int s = -1; s <= 1; s += 2) {
            int x0 = cx + s * (ex - 8), x1 = cx + s * (ex + 8);
            int inner = 0, outer = 0;                           /* heights of the inner and outer ends */
            if (eq(e, "sad") || eq(e, "shy")) { inner = -4; outer = 2; }
            else if (eq(e, "angry")) { inner = 4; outer = -3; }
            else if (eq(e, "surprised") || eq(e, "curious") || eq(e, "listen")) { inner = -3; outer = -3; }
            else if (eq(e, "happy") || eq(e, "love")) { inner = -1; outer = -2; }
            line(cv, x0, by0 + inner, x1, by0 + outer, 2, 0x5C4636);
        }
    }

    /* the mouth: small */
    int my = cy + R / 2;
    if (talking) {
        int open = (int)lround(fabs(sin(t * 2 * M_PI / 0.22)) * 5) + 2;
        ellipse(cv, cx, my + 1, 5, open, C_MOUTH);
        if (open > 4) ellipse(cv, cx, my + 1 + open / 2, 3, open / 3, C_TONGUE);
    } else if (eq(e, "happy") || eq(e, "love") || eq(e, "shy")) {
        bow(cv, cx - 6, cx, my, 4, 2, C_INK);                   /* a little ω */
        bow(cv, cx, cx + 6, my, 4, 2, C_INK);
    } else if (eq(e, "sad") || eq(e, "tired") || eq(e, "dizzy")) {
        bow(cv, cx - 5, cx + 5, my + 3, -3, 2, C_INK);
    } else if (eq(e, "surprised") || eq(e, "puzzled")) {
        ellipse(cv, cx, my + 1, 3, eq(e, "surprised") ? 5 : 3, C_MOUTH);
    } else if (eq(e, "angry")) {
        line(cv, cx - 6, my + 1, cx + 6, my + 1, 2, C_INK);
    } else if (asleep || eq(e, "drowsy")) {
        ellipse(cv, cx, my + 1, 2, 2, C_MOUTH);
    } else {
        bow(cv, cx - 4, cx + 4, my, 2, 2, C_INK);               /* calm, listening, curious, nod */
    }

    /* the small signs above the head */
    int sx = cx + R - 6, sy = cy - R - 8;
    if (eq(e, "puzzled") || eq(e, "curious")) font_text(font, cv, sx, sy - 8, "？", C_TEXT, 0x1A2230);
    else if (eq(e, "surprised")) font_text(font, cv, sx, sy - 8, "！", C_TEXT, 0x1A2230);
    else if (eq(e, "love") || (eq(e, "happy") && f->bond >= 85)) {
        int hy = sy - 4 - (int)lround(3 * sin(t * 2 * M_PI / 1.1));
        disc(cv, sx - 4, hy, 4, C_HEART); disc(cv, sx + 3, hy, 4, C_HEART);
        line(cv, sx - 8, hy + 1, sx, hy + 9, 2, C_HEART); line(cv, sx + 7, hy + 1, sx, hy + 9, 2, C_HEART);
    } else if (eq(e, "tired")) {
        ellipse(cv, cx + R - 8, cy - R / 3, 3, 5, C_SWEAT);
    } else if (asleep) {
        int k = (int)(now / 700) % 3;
        for (int i = 0; i <= k; i++) font_text(font, cv, sx + i * 10, sy - 10 - i * 12, "z", C_Z, 0x1A2230);
    } else if (eq(e, "happy") && f->mood > 60) font_text(font, cv, sx, sy - 8, "♪", C_TEXT, 0x1A2230);

    /* the word under the face */
    snprintf(f->label, sizeof f->label, "%s", talking ? "はなしてる" : label_of(e));
    static const char *const bond_name[] = { "初対面", "顔見知り", "友達", "親友", "家族同然" };
    const char *bn = bond_name[f->bond >= 85 ? 4 : f->bond >= 60 ? 3 : f->bond >= 35 ? 2 : f->bond >= 15 ? 1 : 0];
    char text[64];
    snprintf(text, sizeof text, "%s ・ %s", f->label, bn);
    int tw = font_text_width(font, text);
    font_text(font, cv, x + (FACE_W - tw) / 2, y + FACE_H - 22, text, C_TEXT, 0x1A2230);
}

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

#define C_BODY   0xF2F5FA         /* the white shell */
#define C_BODY_S 0xC9D2E0         /* its shade */
#define C_LINE   0x3B4659         /* the ink of the line art */
#define C_SCREEN 0x1B2535         /* the face screen */
#define C_SCREEN_L 0x273549       /* its rim */
#define C_GLOW   0x5FE4F2         /* the light the face is drawn with */
#define C_GLOW_L 0xC8FAFF
#define C_GLOW_D 0x2BA8BC
#define C_PINK   0xFF8FB0
#define C_BLUE   0x7F9CFF
#define C_YELLOW 0xFFD95A
#define C_BLUSH  0xB54D7A         /* a warm glow on the cheeks */
#define C_SWEAT  0x7FC8FF
#define C_HEART  0xFF6B8A
#define C_TEXT   0xDDE3EA
#define C_DIM    0x7F8894
#define C_Z      0xA0B0C0
#define C_BG     0x1A2230

#define C_SKIN   0xFFEFE4         /* the maid android */
#define C_SKIN_S 0xF3CDBC
#define C_HAIR   0xDCDFF0         /* silver with a touch of lavender */
#define C_HAIR_D 0xA9AEC8
#define C_HAIR_L 0xF6F7FC
#define C_INK    0x3A3346         /* her line art */
#define C_WHITE  0xFFFFFF
#define C_IRIS   0x4FB0CC
#define C_IRIS_D 0x2A6E8C
#define C_IRIS_L 0x9FE3F0
#define C_MOUTH  0xD8707A
#define C_EAR_IN 0xFFC2D1         /* inside the cat ears */
#define C_CHEEK  0xFFB6C1
#define C_RIBBON 0xE36A8A

static const uint32_t c_accent[4] = { 0xBFC8D6, 0x9AD9F5, 0xA8E6C8, 0xFFC66E };   /* a colour that comes with age */

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

/* A filled rounded rectangle from (x0,y0) to (x1,y1) inclusive, corner radius r. */
static void rrect(struct canvas *cv, int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    int w = x1 - x0, h = y1 - y0;
    if (w < 0 || h < 0) return;
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    for (int y = y0; y <= y1; y++) {
        int inset = 0;
        if (y < y0 + r || y > y1 - r) {
            int dy = y < y0 + r ? y0 + r - y : y - (y1 - r);
            double f = 1.0 - (double)dy * dy / ((double)r * r);
            inset = r - (int)(r * sqrt(f < 0 ? 0 : f) + 0.5);
        }
        for (int x = x0 + inset; x <= x1 - inset; x++) put(cv, x, y, c);
    }
}

/* A small heart with its point at (cx, cy + r). */
static void heart(struct canvas *cv, int cx, int cy, int r, uint32_t c)
{
    disc(cv, cx - r / 2, cy - r / 3, r / 2 + 1, c);
    disc(cv, cx + r / 2, cy - r / 3, r / 2 + 1, c);
    tri(cv, cx - r, cy - r / 4, cx + r, cy - r / 4, cx, cy + r, c);
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
            else if (strcmp(key, "model") == 0) f->model = atoi(val);
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
    f->model = f->model < 0 ? 0 : f->model > 1 ? 1 : f->model;
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

/* Everything about the frame at `now` that is not in the state itself,
 * hashed: the breathing, the blink, the mouth, the lamps, the signs. */
static uint32_t pose_of(const struct face *f, long now)
{
    double t = now / 1000.0;
    const char *e = face_expr(f, now);
    int talking = face_talking(f, now);
    long since_blink = now - f->blink_at;
    int bob = (int)lround(2.0 * sin(t * 2 * M_PI / 2.4));
    if (talking) bob += (int)lround(1.5 * sin(t * 2 * M_PI / 0.3));
    if (strcmp(e, "dizzy") == 0) bob += (int)lround(2.0 * sin(t * 2 * M_PI / 0.15));
    int sway = (int)lround(2 * sin(t * 2 * M_PI / 1.7));
    int pulse = (int)lround(1.0 + sin(t * 2 * M_PI / (talking ? 0.4 : 2.4)));
    int blink = since_blink < 180 ? (int)(since_blink / 45) : 9;
    int mouth = talking ? (int)(now / 40) : 0;
    int sign = strcmp(e, "sleep") == 0 ? (int)(now / 700) % 3 : (int)lround(3 * sin(t * 2 * M_PI / 1.1));
    uint32_t h = 2166136261u;
    const int v[] = { bob, sway, pulse, blink, mouth, sign, f->look_x, f->look_y, talking, f->mood, f->bond, f->stage, f->body, f->model };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) h = (h ^ (uint32_t)(v[i] + 1000)) * 16777619u;
    for (const char *c = e; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    return h;
}

int face_tick(struct face *f, long now)
{
    if (now >= f->blink_next) { f->blink_at = now; f->blink_next = now + 2500 + (long)(rnd(f) % 3500); }
    if (f->look_x != f->target_x) f->look_x += f->look_x < f->target_x ? 1 : -1;
    if (f->look_y != f->target_y) f->look_y += f->look_y < f->target_y ? 1 : -1;
    return pose_of(f, now) != f->pose;          /* only a frame that would look different */
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

/* ---- the robot: a white rounded head, a face screen, an antenna lamp */
static void draw_robot(struct face *f, struct canvas *cv, const char *e, int talking, int asleep, double t,
                       int cx, int cy, int tilt, long now, int *sx_out, int *sy_out)
{
    int st = f->stage;
    (void)now;
    int happy = eq(e, "happy") || eq(e, "love") || eq(e, "shy");
    int HW = 66 + 3 * st, HH = 52 + 2 * st;                           /* the head: a rounded box, bigger with age */
    uint32_t acc = c_accent[st];

    /* the antenna: a stem and a lamp that glows with the mood */
    int ant_x = cx + (int)lround(2 * sin(t * 2 * M_PI / 1.7)) + tilt / 2, ant_top = cy - HH - 16;
    line(cv, cx, cy - HH + 2, ant_x, ant_top + 4, 4, C_LINE);
    line(cv, cx, cy - HH + 2, ant_x, ant_top + 4, 2, C_BODY_S);
    uint32_t lamp = eq(e, "sad") || eq(e, "tired") ? C_BLUE : happy ? C_PINK
                  : eq(e, "curious") || eq(e, "puzzled") || eq(e, "surprised") ? C_YELLOW : asleep ? C_DIM : C_GLOW;
    int pulse = asleep ? 0 : (int)lround(1.0 + sin(t * 2 * M_PI / (talking ? 0.4 : 2.4)));
    if (eq(e, "listen")) { lamp = C_GLOW_L; pulse = 2; }                       /* all ears, so to speak */
    disc(cv, ant_x, ant_top - 1, 6 + pulse, C_LINE);
    disc(cv, ant_x, ant_top - 1, 4 + pulse, lamp);
    disc(cv, ant_x - 1, ant_top - 3, 1, C_GLOW_L);

    /* the head and its screen */
    rrect(cv, cx - HW - 2, cy - HH - 2, cx + HW + 2, cy + HH + 2, 30, C_LINE);
    rrect(cv, cx - HW, cy - HH, cx + HW, cy + HH, 28, C_BODY);
    rrect(cv, cx - HW + 24, cy + HH - 9, cx + HW - 24, cy + HH - 5, 3, st >= 1 ? acc : C_BODY_S);   /* the chin: a stripe of the age's colour */
    int SW = HW - 10, SH = HH - 10;
    rrect(cv, cx - SW - 1, cy - SH - 1, cx + SW + 1, cy + SH + 1, 22, C_LINE);
    rrect(cv, cx - SW, cy - SH, cx + SW, cy + SH, 21, C_SCREEN_L);
    rrect(cv, cx - SW + 2, cy - SH + 2, cx + SW - 2, cy + SH - 2, 19, C_SCREEN);
    if (st >= 3) {                                                    /* a star on the forehead corner */
        int sx0 = cx - SW + 14, sy0 = cy - SH + 12;
        for (int k = 0; k < 5; k++) {
            double a0 = (k * 72 - 90) * M_PI / 180, a1 = ((k * 72 + 36) - 90) * M_PI / 180, a2 = ((k + 1) * 72 - 90) * M_PI / 180;
            tri(cv, sx0 + (int)lround(6 * cos(a0)), sy0 + (int)lround(6 * sin(a0)), sx0 + (int)lround(2 * cos(a1)), sy0 + (int)lround(2 * sin(a1)), sx0, sy0, C_YELLOW);
            tri(cv, sx0 + (int)lround(6 * cos(a2)), sy0 + (int)lround(6 * sin(a2)), sx0 + (int)lround(2 * cos(a1)), sy0 + (int)lround(2 * sin(a1)), sx0, sy0, C_YELLOW);
        }
    }

    /* the cheeks glow from 友達 */
    int rosy = f->bond >= 35 || eq(e, "shy") || eq(e, "love");
    if (rosy && !asleep) {
        ellipse(cv, cx - SW + 18, cy + 12, 9, 4, C_BLUSH);
        ellipse(cv, cx + SW - 18, cy + 12, 9, 4, C_BLUSH);
    }

    /* the eyes, drawn in light */
    int ex0 = 24 + 3 * st, ey0 = cy - 6 + f->look_y;
    long since_blink = now - f->blink_at;
    double blink = since_blink < 180 ? 1.0 - labs(since_blink - 90) / 90.0 : 0;   /* 0 open .. 1 closed */
    int closed = asleep || eq(e, "nod") || blink > 0.75;
    int half = eq(e, "drowsy") || eq(e, "tired");
    int wide = eq(e, "surprised") || eq(e, "listen") || eq(e, "curious");
    int rx0 = 10 + st, ry0 = 12 + st;
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        int ecx = cx + sgn * ex0 + f->look_x, ecy = ey0;
        if (eq(e, "dizzy")) {                                   /* spirals */
            arc(cv, ecx, ecy, 9, 9, 0, 300, 3, C_GLOW);
            arc(cv, ecx, ecy, 5, 5, 60, 360, 3, C_GLOW);
            continue;
        }
        if (eq(e, "love")) { heart(cv, ecx, ecy, rx0 + 2, C_PINK); disc(cv, ecx - 3, ecy - 4, 2, C_GLOW_L); continue; }
        if (happy && !closed) {                                 /* smiling eyes: ^ ^ */
            arc(cv, ecx, ecy + 4, rx0 + 1, ry0 - 2, 200, 340, 4, C_GLOW);
            continue;
        }
        if (closed) {
            bow(cv, ecx - rx0, ecx + rx0, ecy + 2, asleep ? 3 : 2, 3, C_GLOW_D);
            continue;
        }
        if (eq(e, "puzzled") && sgn > 0) { rx0 -= 3; ry0 -= 4; }   /* one eye squints */
        int rx = rx0 + (wide ? 2 : 0), ry = (int)lround((ry0 + (wide ? 3 : 0)) * (1.0 - blink * 0.9));
        if (half) ry = ry / 2 + 1;
        if (ry < 2) ry = 2;
        ellipse(cv, ecx, ecy, rx + 1, ry + 1, C_GLOW_D);
        ellipse(cv, ecx, ecy, rx, ry, C_GLOW);
        ellipse(cv, ecx, ecy + ry / 3, rx - 3, ry / 2, C_GLOW_L);  /* the soft core */
        if (eq(e, "sad")) tri(cv, ecx - sgn * (rx + 2), ecy - ry - 2, ecx + sgn * (rx + 2), ecy - ry - 2, ecx + sgn * (rx + 2), ecy - 1, C_SCREEN); /* the lids droop outward */
        if (eq(e, "angry")) tri(cv, ecx - sgn * (rx + 2), ecy - ry - 2, ecx + sgn * (rx + 2), ecy - ry - 2, ecx - sgn * (rx + 2), ecy - 1, C_SCREEN); /* ... inward */
        if (half) canvas_fill(cv, ecx - rx - 1, ecy - ry - 1, 2 * rx + 3, ry, C_SCREEN);
        if (eq(e, "surprised")) disc(cv, ecx, ecy, 3, C_SCREEN);
        if (eq(e, "puzzled") && sgn > 0) { rx0 += 3; ry0 += 4; }
    }

    /* the mouth, also in light */
    int my = cy + 22;
    if (talking) {                                              /* a voice: bars that dance */
        for (int k = -2; k <= 2; k++) {
            int h = 2 + (int)lround(4 * fabs(sin(t * 2 * M_PI / (0.17 + 0.03 * (k + 2)) + k)));
            canvas_fill(cv, cx + k * 5 - 1, my - h, 3, 2 * h + 1, k == 0 ? C_GLOW_L : C_GLOW);
        }
    } else if (happy) {
        bow(cv, cx - 9, cx + 9, my - 2, 5, 3, C_GLOW);
    } else if (eq(e, "sad") || eq(e, "tired") || eq(e, "dizzy")) {
        bow(cv, cx - 7, cx + 7, my + 2, -4, 3, C_GLOW);
    } else if (eq(e, "surprised")) {
        ellipse(cv, cx, my, 5, 6, C_GLOW); ellipse(cv, cx, my, 3, 4, C_SCREEN);
    } else if (eq(e, "puzzled")) {
        bow(cv, cx - 7, cx + 7, my, 3, 3, C_GLOW);               /* a wobble */
        bow(cv, cx - 7, cx, my, -3, 3, C_SCREEN); bow(cv, cx - 7, cx, my + 1, -3, 3, C_GLOW);
    } else if (eq(e, "angry")) {
        line(cv, cx - 8, my, cx + 8, my, 3, C_GLOW);
    } else if (asleep || eq(e, "drowsy")) {
        disc(cv, cx, my, 2, C_GLOW_D);
    } else {
        bow(cv, cx - 6, cx + 6, my - 1, 2, 3, C_GLOW);           /* calm, listening, curious, nod */
    }

    *sx_out = cx + HW - 2; *sy_out = cy - HH - 12;
}

/* ---- the maid android: a soft face, silver hair with cat ears, a frilled
 * headdress, an intercom on one ear; her eyes are anime eyes */
static void draw_maid(struct face *f, struct canvas *cv, const char *e, int talking, int asleep, double t,
                      int cx, int cy, int tilt, long now, int *sx_out, int *sy_out)
{
    int st = f->stage;
    int happy = eq(e, "happy") || eq(e, "love") || eq(e, "shy");
    int RX = 50 + 2 * st, RY = 46 + 2 * st;                           /* the face: a soft oval, a smaller chin oval */
    int top = cy - RY;
    int listen = eq(e, "listen");
    (void)tilt;

    /* hair behind the head: a soft mass that sits high and stops at the cheeks */
    {
        struct clip saved = g_clip;
        if (g_clip.y1 > cy + RY / 3) g_clip.y1 = cy + RY / 3;
        ellipse(cv, cx, cy - 10, RX + 12, RY + 8, C_INK);
        ellipse(cv, cx, cy - 10, RX + 10, RY + 6, C_HAIR_D);
        g_clip = saved;
    }
    /* side locks down the cheeks, longer with age */
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        int lx = cx + sgn * (RX + 2), ly = cy + 4 + 4 * st;
        ellipse(cv, lx, ly, 9, 18 + 5 * st, C_INK);
        ellipse(cv, lx, ly, 7, 16 + 5 * st, C_HAIR);
    }
    /* cat ears: they perk up when listening, droop when sad */
    int perk = listen ? 6 : eq(e, "sad") || eq(e, "tired") || asleep ? -5 : 0;
    int wig = listen ? (int)lround(2 * sin(t * 2 * M_PI / 0.5)) : 0;
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        int bx = cx + sgn * 30, by = top + 6, tx = cx + sgn * (46 + wig), ty = top - 26 - perk;
        tri(cv, bx - sgn * 14 - 1, by, bx + sgn * 12 + 1, by + 4, tx + sgn * 2, ty - 2, C_INK);
        tri(cv, bx - sgn * 14, by, bx + sgn * 12, by + 4, tx, ty, C_HAIR);
        tri(cv, bx - sgn * 8, by + 1, bx + sgn * 8, by + 3, tx - sgn * 2, ty + 9, C_EAR_IN);
    }
    /* the face */
    ellipse(cv, cx, cy, RX + 2, RY + 2, C_INK);
    ellipse(cv, cx, cy + RY / 3, (int)(RX * 0.78) + 2, (int)(RY * 0.62) + 2, C_INK);
    ellipse(cv, cx, cy, RX, RY, C_SKIN);
    ellipse(cv, cx, cy + RY / 3, (int)(RX * 0.78), (int)(RY * 0.62), C_SKIN);
    /* the hair on top: a cap down to the fringe, then round locks of a fringe */
    {
        struct clip saved = g_clip;
        int hl = top + 14;
        if (g_clip.y1 > hl + 6) g_clip.y1 = hl + 6;
        ellipse(cv, cx, cy - 6, RX + 4, RY + 4, C_INK);
        ellipse(cv, cx, cy - 6, RX + 2, RY + 2, C_HAIR);
        g_clip = saved;
        int nb = 5, w = (2 * RX - 10) / nb;
        for (int i = 0; i < nb; i++) {                                 /* round locks: hair first ... */
            int bxx = cx - RX + 5 + w / 2 + i * w, byy = hl + (i % 2 ? 0 : 4);
            ellipse(cv, bxx, byy, w / 2 + 2, 12, C_HAIR);
        }
        for (int i = 0; i < nb; i++)                                   /* ... then only their lower edge in ink */
            arc(cv, cx - RX + 5 + w / 2 + i * w, hl + (i % 2 ? 0 : 4), w / 2 + 2, 12, 15, 165, 1, C_INK);
        arc(cv, cx - 10, hl - 2, 22, 6, 200, 300, 2, C_HAIR_L);                 /* a shine */
    }
    /* the headdress: a white frilled band between the ears */
    {
        int hy = top - 2;
        bow(cv, cx - 26, cx + 26, hy + 6, -8, 7, C_INK);
        bow(cv, cx - 26, cx + 26, hy + 6, -8, 4, C_WHITE);
        for (int i = -3; i <= 3; i++) {
            int fx = cx + i * 8, fy = hy + 6 - (int)lround(8 * 4 * (0.5 + i / 7.0) * (0.5 - i / 7.0)) - 3;
            disc(cv, fx, fy, 3, C_INK); disc(cv, fx, fy, 2, C_WHITE);
        }
        disc(cv, cx, hy + 1, 4, C_INK); disc(cv, cx, hy + 1, 3, C_RIBBON);        /* a small ribbon knot */
    }
    /* the intercom: an ear piece on her left, a mic boom toward the mouth */
    {
        int ex = cx - RX + 2, ey = cy + 6;
        disc(cv, ex, ey, 8, C_INK);
        disc(cv, ex, ey, 6, C_LINE);
        disc(cv, ex, ey, 2, talking ? C_GLOW_L : listen ? C_GLOW : C_GLOW_D);      /* its lamp */
        arc(cv, ex + 14, ey + 10, 16, 18, 120, 180, 2, C_INK);                     /* the boom */
        disc(cv, ex + 14 - 2, ey + 28, 3, C_INK);
        disc(cv, ex + 14 - 2, ey + 28, 2, talking ? C_GLOW_L : C_LINE);
    }
    /* an android: a thin panel seam on the right cheek */
    line(cv, cx + RX - 16, cy + 18, cx + RX - 9, cy + 18, 1, C_IRIS_D);

    /* the cheeks from 友達, or when shy */
    if ((f->bond >= 35 || eq(e, "shy") || eq(e, "love")) && !asleep) {
        ellipse(cv, cx - 28, cy + 12, 9, 4, C_CHEEK);
        ellipse(cv, cx + 28, cy + 12, 9, 4, C_CHEEK);
    }

    /* the eyes */
    int ex0 = 22 + st, ey0 = cy - 2 + f->look_y;
    long since_blink = now - f->blink_at;
    double blink = since_blink < 180 ? 1.0 - labs(since_blink - 90) / 90.0 : 0;
    int closed = asleep || eq(e, "nod") || blink > 0.75;
    int half = eq(e, "drowsy") || eq(e, "tired");
    int wide = eq(e, "surprised") || listen || eq(e, "curious");
    int rx0 = 10 + st / 2, ry0 = 13 + st / 2;
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        int ecx = cx + sgn * ex0 + f->look_x, ecy = ey0;
        /* the brow */
        int bi = eq(e, "sad") ? -3 : eq(e, "angry") ? 4 : eq(e, "surprised") ? -4 : 0;    /* the inner end: up (-) or down */
        line(cv, ecx - sgn * 9, ecy - ry0 - 8 + bi, ecx + sgn * 9, ecy - ry0 - 8, 2, C_HAIR_D);
        if (eq(e, "dizzy")) {
            arc(cv, ecx, ecy, 9, 9, 0, 300, 2, C_INK);
            arc(cv, ecx, ecy, 5, 5, 60, 360, 2, C_INK);
            continue;
        }
        if (eq(e, "love")) { heart(cv, ecx, ecy, rx0 + 2, C_MOUTH); disc(cv, ecx - 3, ecy - 4, 2, C_WHITE); continue; }
        if (happy && !closed) {
            arc(cv, ecx, ecy + 4, rx0 + 1, ry0 - 3, 200, 340, 3, C_INK);
            line(cv, ecx + sgn * (rx0 + 1), ecy - 1, ecx + sgn * (rx0 + 5), ecy - 5, 3, C_INK);   /* the lash */
            continue;
        }
        if (closed) {
            bow(cv, ecx - rx0, ecx + rx0, ecy + 1, asleep ? 4 : 3, 3, C_INK);
            line(cv, ecx + sgn * (rx0 + 1), ecy + 1, ecx + sgn * (rx0 + 5), ecy - 3, 3, C_INK);
            continue;
        }
        if (eq(e, "puzzled") && sgn > 0) { rx0 -= 2; ry0 -= 4; }
        int rx = rx0 + (wide ? 1 : 0), ry = (int)lround((ry0 + (wide ? 3 : 0)) * (1.0 - blink * 0.9));
        if (half) ry = ry / 2 + 2;
        if (ry < 3) ry = 3;
        ellipse(cv, ecx, ecy, rx + 2, ry + 2, C_INK);
        ellipse(cv, ecx, ecy, rx, ry, C_WHITE);
        int ir = rx - 3, iry = ry - 2;
        ellipse(cv, ecx, ecy + 1, ir + 1, iry + 1, C_IRIS_D);
        ellipse(cv, ecx, ecy + 1, ir, iry, C_IRIS);
        ellipse(cv, ecx, ecy + 1 + iry / 2, ir - 2, iry / 2, C_IRIS_L);
        ellipse(cv, ecx, ecy + 1, ir / 2, iry / 2 + 1, C_INK);
        if (eq(e, "surprised")) ellipse(cv, ecx, ecy + 1, ir / 3, iry / 3, C_INK);
        disc(cv, ecx - 3, ecy - iry / 2, 3, C_WHITE);                              /* the highlights */
        disc(cv, ecx + 3, ecy + iry / 2, 1, C_WHITE);
        arc(cv, ecx, ecy, rx + 1, ry + 1, 200, 340, 3, C_INK);                     /* the upper lid */
        line(cv, ecx + sgn * (rx + 1), ecy - 2, ecx + sgn * (rx + 5), ecy - 6, 3, C_INK);
        if (eq(e, "sad")) tri(cv, ecx - sgn * (rx + 3), ecy - ry - 3, ecx + sgn * (rx + 3), ecy - ry - 3, ecx + sgn * (rx + 3), ecy - 1, C_SKIN);
        if (eq(e, "angry")) tri(cv, ecx - sgn * (rx + 3), ecy - ry - 3, ecx + sgn * (rx + 3), ecy - ry - 3, ecx - sgn * (rx + 3), ecy - 1, C_SKIN);
        if (half) { canvas_fill(cv, ecx - rx - 2, ecy - ry - 3, 2 * rx + 5, ry + 1, C_SKIN); line(cv, ecx - rx, ecy - 2, ecx + rx, ecy - 2, 3, C_INK); }
        if (eq(e, "puzzled") && sgn > 0) { rx0 += 2; ry0 += 4; }
    }

    /* the mouth: small */
    int my = cy + RY / 2 + 2;
    if (talking) {
        int open = (int)lround(fabs(sin(t * 2 * M_PI / 0.22)) * 5) + 2;
        ellipse(cv, cx, my + 1, 5, open + 1, C_INK);
        ellipse(cv, cx, my + 1, 4, open, C_MOUTH);
    } else if (happy) {
        tri(cv, cx - 6, my - 2, cx + 6, my - 2, cx, my + 5, C_INK);
        tri(cv, cx - 4, my - 1, cx + 4, my - 1, cx, my + 3, C_MOUTH);
    } else if (eq(e, "sad") || eq(e, "tired") || eq(e, "dizzy")) {
        bow(cv, cx - 5, cx + 5, my + 2, -3, 2, C_INK);
    } else if (eq(e, "surprised") || eq(e, "puzzled")) {
        ellipse(cv, cx, my + 1, 3, eq(e, "surprised") ? 5 : 3, C_INK);
        ellipse(cv, cx, my + 1, 2, eq(e, "surprised") ? 4 : 2, C_MOUTH);
    } else if (eq(e, "angry")) {
        line(cv, cx - 6, my + 1, cx + 6, my + 1, 2, C_INK);
    } else if (asleep || eq(e, "drowsy")) {
        ellipse(cv, cx, my + 1, 2, 2, C_MOUTH);
    } else {
        bow(cv, cx - 4, cx + 4, my, 2, 2, C_INK);
    }
    *sx_out = cx + RX + 6; *sy_out = top - 30;
}

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
    int sx, sy;                                                       /* where the signs go: above the head's right */
    f->pose = pose_of(f, now);
    int cx = x + FACE_W / 2 + tilt, cy = y + 100 + bob;
    if (f->model == 1) draw_maid(f, cv, e, talking, asleep, t, cx, cy, tilt, now, &sx, &sy);
    else draw_robot(f, cv, e, talking, asleep, t, cx, cy, tilt, now, &sx, &sy);

    /* the small signs above the head */
    if (eq(e, "puzzled") || eq(e, "curious")) font_text(font, cv, sx, sy - 8, "？", C_TEXT, C_BG);
    else if (eq(e, "surprised")) font_text(font, cv, sx, sy - 8, "！", C_TEXT, C_BG);
    else if (eq(e, "love") || (eq(e, "happy") && f->bond >= 85)) {
        int hy = sy - 4 - (int)lround(3 * sin(t * 2 * M_PI / 1.1));
        heart(cv, sx, hy, 7, C_HEART);
    } else if (eq(e, "tired")) {
        ellipse(cv, sx - 10, sy + 24, 3, 5, C_SWEAT);
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

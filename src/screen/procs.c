/* The processes as a picture (see procs.h): read from /proc, laid out as
 * rings around PID 1, drawn with circles and lines. */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "procs.h"

#define C_BG       0x151B24
#define C_EDGE     0x34455C
#define C_EDGE_HOT 0x6A8BB5
#define C_RUN      0xFFA64D         /* R: running right now */
#define C_SLEEP    0x5FA8FF         /* S: waiting */
#define C_DISK     0xFF5F5F         /* D: waiting for the disk */
#define C_ZOMBIE   0x8A94A0         /* Z */
#define C_STOP     0xC678DD         /* T */
#define C_IDLE     0x4F6A8F         /* I: an idle kernel thread */
#define C_KERNEL   0x5A6B82
#define C_CORE     0xF2F5FA         /* PID 1 */
#define C_CPU      0xFFE08A         /* the CPU ring */
#define C_SPARK    0xFFF3C4
#define C_FLASH    0x9AD9F5
#define C_TEXT     0xDDE3EA
#define C_TEXT_DIM 0x8A94A0
#define C_HOVER    0xFFFFFF
#define LEGEND_H   (FONT_H + 6)
#define STATUS_H   (FONT_H + 6)
#define FLASH_MS   900
#define SPARK_MS   1200

/* ---- drawing primitives, clipped to the window */

struct clip { int x0, y0, x1, y1; };
static struct clip g_clip;

static void put(struct canvas *cv, int x, int y, uint32_t c)
{
    if (x < g_clip.x0 || y < g_clip.y0 || x >= g_clip.x1 || y >= g_clip.y1) return;
    cv->px[(size_t)y * cv->w + x] = c;
}

static void disc(struct canvas *cv, int cx, int cy, int r, uint32_t c)
{
    if (r <= 0) { put(cv, cx, cy, c); return; }
    for (int y = -r; y <= r; y++) {
        int hw = (int)(sqrt((double)r * r - (double)y * y) + 0.5);
        for (int x = -hw; x <= hw; x++) put(cv, cx + x, cy + y, c);
    }
}

static void line(struct canvas *cv, int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    if (n == 0) { put(cv, x0, y0, c); return; }
    for (int i = 0; i <= n; i++) put(cv, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, c);
}

/* The outline of a circle, `thick` pixels wide; `a0`..`a1` in degrees (0 = right, 90 = down). */
static void ring(struct canvas *cv, int cx, int cy, int r, double a0, double a1, int thick, uint32_t c)
{
    if (r <= 0) return;
    int steps = (int)(fabs(a1 - a0) * r / 60) + 2;
    for (int i = 0; i <= steps; i++) {
        double a = (a0 + (a1 - a0) * i / steps) * M_PI / 180.0;
        int x = cx + (int)lround(r * cos(a)), y = cy + (int)lround(r * sin(a));
        if (thick <= 1) put(cv, x, y, c); else disc(cv, x, y, thick / 2, c);
    }
}

/* ---- reading /proc */

static uint32_t state_color(char st)
{
    switch (st) {
    case 'R': return C_RUN;
    case 'D': return C_DISK;
    case 'Z': case 'X': return C_ZOMBIE;
    case 'T': case 't': return C_STOP;
    case 'I': return C_IDLE;
    case 'K': return C_KERNEL;
    default: return C_SLEEP;
    }
}

static const char *state_word(char st)
{
    switch (st) {
    case 'R': return "動いている";
    case 'D': return "ディスク待ち";
    case 'Z': case 'X': return "ゾンビ";
    case 'T': case 't': return "止まっている";
    case 'I': return "休んでいる";
    case 'K': return "カーネルの中";
    default: return "待っている";
    }
}

/* One /proc/<pid>/stat line into `n`; 0 when it cannot be read. */
static int read_stat(const char *root, int pid, struct proc_node *n)
{
    char path[600], buf[1024];
    snprintf(path, sizeof path, "%s/%d/stat", root, pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = '\0';
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp || rp < lp) return 0;
    memset(n, 0, sizeof *n);
    n->pid = pid;
    size_t nl = (size_t)(rp - lp - 1);
    if (nl >= sizeof n->name) nl = sizeof n->name - 1;
    memcpy(n->name, lp + 1, nl);
    n->name[nl] = '\0';
    /* after the name: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime ... rss is the 22nd */
    char st = ' ';
    int ppid = 0;
    unsigned long ut = 0, stt = 0;
    long rss = 0;
    if (sscanf(rp + 1, " %c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
               &st, &ppid, &ut, &stt, &rss) < 2) return 0;
    n->state = st;
    n->ppid = ppid;
    n->ticks = ut + stt;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    n->rss_kb = rss * (page_kb > 0 ? page_kb : 4);
    n->parent = -1;
    return 1;
}

static int by_pid(const void *a, const void *b)
{
    const struct proc_node *x = a, *y = b;
    return x->pid < y->pid ? -1 : x->pid > y->pid;
}

static int index_of(const struct procs *p, int pid)
{
    for (int i = 0; i < p->count; i++) if (p->n[i].pid == pid) return i;
    return -1;
}

static int bubble_radius(const struct proc_node *n)
{
    if (n->state == 'K') { int r = 7 + n->kernel / 12; return r > 16 ? 16 : r; }
    int r = 5 + (int)(1.4 * log2((double)n->rss_kb / 128.0 + 1.0));
    if (n->pid == 1 && r < 11) r = 11;
    return r < 5 ? 5 : r > 18 ? 18 : r;
}

/* Where every bubble belongs: rings by depth, sectors by how many leaves hang below. */
static void layout(struct procs *p)
{
    int plot_y = LEGEND_H, plot_h = p->h - LEGEND_H - STATUS_H;
    int cx = p->w / 2, cy = plot_y + plot_h / 2;
    /* the rings are ellipses filling the window; a tree deeper than PROCS_RINGS spills a little past the last ring */
    int rx = p->w / 2 - 80, ry = plot_h / 2 - 18;              /* room for a name beside the outer bubbles */
    if (rx < 20) rx = 20;
    if (ry < 20) ry = 20;
    /* depth: how many parents up to the centre */
    int maxd = 0;
    for (int i = 0; i < p->count; i++) {
        struct proc_node *n = &p->n[i];
        n->depth = 0;
        n->leaves = 0;
        n->nchild = 0;
        if (n->pid == 1) continue;
        int d = 1, k = n->parent;
        while (k >= 0 && p->n[k].pid != 1 && d < 64) { k = p->n[k].parent; d++; }
        n->depth = d;
        if (d > maxd) maxd = d;
    }
    for (int i = 0; i < p->count; i++) if (p->n[i].parent >= 0) p->n[p->n[i].parent].nchild++;
    /* leaves, from the deepest up */
    for (int d = maxd; d >= 1; d--)
        for (int i = 0; i < p->count; i++) {
            struct proc_node *n = &p->n[i];
            if (n->depth != d) continue;
            if (n->nchild == 0) n->leaves = 1;
            if (n->parent >= 0 && p->n[n->parent].pid != 1) p->n[n->parent].leaves += n->leaves;
        }
    int total = 0;
    for (int i = 0; i < p->count; i++) if (p->n[i].depth == 1) total += p->n[i].leaves;
    if (total < 1) total = 1;
    /* sectors: the first ring shares the circle, each deeper node shares its parent's sector */
    double *s0 = calloc((size_t)p->count * 2 + 2, sizeof *s0);
    if (!s0) return;
    double *s1 = s0 + p->count;
    double a = -M_PI / 2;                                /* the first child goes up */
    for (int i = 0; i < p->count; i++) {
        struct proc_node *n = &p->n[i];
        if (n->depth != 1) continue;
        s0[i] = a;
        a += 2 * M_PI * n->leaves / total;
        s1[i] = a;
        n->angle = (s0[i] + s1[i]) / 2;
    }
    for (int d = 2; d <= maxd; d++)
        for (int i = 0; i < p->count; i++) {
            struct proc_node *par = &p->n[i];
            if (par->depth != d - 1 || par->nchild == 0) continue;
            double b = s0[i], span = s1[i] - s0[i];
            int leaves = par->leaves > 0 ? par->leaves : 1;
            for (int j = 0; j < p->count; j++) {
                struct proc_node *n = &p->n[j];
                if (n->parent != i || n->depth != d) continue;
                s0[j] = b;
                b += span * n->leaves / leaves;
                s1[j] = b;
                n->angle = (s0[j] + s1[j]) / 2;
                if (par->nchild == 1) n->angle += (d & 1 ? 0.22 : -0.22);     /* a chain zigzags instead of hiding behind itself */
            }
        }
    free(s0);
    int rings = maxd < 2 ? 2 : maxd > PROCS_RINGS ? PROCS_RINGS : maxd;      /* a shallow tree spreads out */
    if (maxd > rings) { rx = (int)(rx / 1.15); ry = (int)(ry / 1.15); }        /* a deeper one spills past the last ring */
    for (int i = 0; i < p->count; i++) {
        struct proc_node *n = &p->n[i];
        double k = n->depth <= rings ? (double)n->depth / rings : 1.0 + 0.05 * (n->depth - rings);
        if (k > 1.15) k = 1.15;
        n->tx = cx + (int)lround(rx * k * cos(n->angle));
        n->ty = cy + (int)lround(ry * k * sin(n->angle));
        n->r = bubble_radius(n);
        if (rings > 3) n->r = n->r * 2 / 3 < 4 ? 4 : n->r * 2 / 3;             /* a deep tree: smaller bubbles so that the rings stay apart */
    }
}

void procs_init(struct procs *p, int w, int h)
{
    memset(p, 0, sizeof *p);
    p->w = w;
    p->h = h;
    p->root = "/proc";
    p->hover = -1;
    p->hot = -1;
    p->px = p->py = -1000;
    long hz = sysconf(_SC_CLK_TCK);
    p->tick_hz = hz > 0 ? hz : 100;
}

int procs_sample(struct procs *p, long now)
{
    struct proc_node old[PROCS_MAX];
    int nold = p->count;
    memcpy(old, p->n, sizeof(struct proc_node) * (size_t)nold);
    long dt = p->sampled ? now - p->sampled : 0;

    DIR *d = opendir(p->root);
    p->count = p->total = p->running = p->kernel = 0;
    struct proc_node kern;
    memset(&kern, 0, sizeof kern);
    snprintf(kern.name, sizeof kern.name, "カーネル");
    kern.state = 'K';
    kern.pid = 2;
    kern.ppid = 1;
    kern.parent = -1;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            char *end;
            long pid = strtol(e->d_name, &end, 10);
            if (*end || pid <= 0 || pid > 4194304) continue;
            struct proc_node n;
            if (!read_stat(p->root, (int)pid, &n)) continue;
            p->total++;
            if (n.state == 'R') p->running++;
            if (n.pid == 2 || n.ppid == 2) {                 /* a kernel thread: folded */
                kern.kernel++;
                kern.ticks += n.ticks;
                continue;
            }
            if (p->count < PROCS_MAX - 1) p->n[p->count++] = n;
        }
        closedir(d);
    }
    if (kern.kernel) { p->kernel = kern.kernel; p->n[p->count++] = kern; }
    qsort(p->n, (size_t)p->count, sizeof p->n[0], by_pid);
    /* link to the parents; the kernel node hangs off PID 1 */
    for (int i = 0; i < p->count; i++) {
        struct proc_node *n = &p->n[i];
        n->parent = n->pid == 1 ? -1 : index_of(p, n->ppid);
        if (n->parent == i) n->parent = -1;
    }
    layout(p);
    /* CPU since the last look, and where each bubble was, by pid */
    p->hot = -1;
    long best = -1;
    unsigned long best_ticks = 0;
    for (int i = 0; i < p->count; i++) {
        struct proc_node *n = &p->n[i];
        int o = -1;
        for (int j = 0; j < nold; j++) if (old[j].pid == n->pid) { o = j; break; }
        if (o >= 0) {
            n->x = old[o].x;
            n->y = old[o].y;
            n->born = old[o].born;
            if (dt > 0 && n->ticks >= old[o].ticks)
                n->cpu = (int)((long long)(n->ticks - old[o].ticks) * 1000LL * 1000LL / ((long long)p->tick_hz * dt));
            if (n->cpu > 1000) n->cpu = 1000;
        } else if (nold) {                                   /* new: it grows out of its parent */
            int k = n->parent;
            n->x = k >= 0 ? p->n[k].tx : p->w / 2;
            n->y = k >= 0 ? p->n[k].ty : LEGEND_H + (p->h - LEGEND_H - STATUS_H) / 2;
            n->born = now;
        } else {                                             /* the first picture: everything in place, no flash */
            n->x = n->tx;
            n->y = n->ty;
            n->born = now - FLASH_MS;
        }
        if (n->state == 'K') continue;
        if (n->cpu > best || (n->cpu == best && n->ticks > best_ticks)) { best = n->cpu; best_ticks = n->ticks; p->hot = i; }
    }
    p->sample_dt = dt;
    p->sampled = now;
    p->settled = 0;
    return p->count;
}

/* Everything the next frame depends on, folded into a number. */
static uint32_t pose_of(const struct procs *p, long now)
{
    uint32_t h = 2166136261u;
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    MIX(p->count); MIX(p->hover); MIX(p->sampled);
    for (int i = 0; i < p->count; i++) {
        const struct proc_node *n = &p->n[i];
        MIX(n->x); MIX(n->y); MIX(n->state);
        if (n->state == 'R' || n->cpu > 0) MIX(now / 120);              /* pulse and spark move */
        if (now - n->born < FLASH_MS) MIX(now / 40);                     /* the arrival ring grows */
    }
#undef MIX
    return h;
}

int procs_tick(struct procs *p, long now)
{
    if (p->sampled && now - p->sampled >= PROCS_SAMPLE_MS) return 1;   /* time to look again (procs_draw does) */
    if (!p->settled) {
        int moving = 0;
        for (int i = 0; i < p->count; i++) {
            struct proc_node *n = &p->n[i];
            int dx = n->tx - n->x, dy = n->ty - n->y;
            if (abs(dx) <= 1 && abs(dy) <= 1) { n->x = n->tx; n->y = n->ty; continue; }
            n->x += dx / 3 + (dx > 0 ? 1 : -1);
            n->y += dy / 3 + (dy > 0 ? 1 : -1);
            moving = 1;
        }
        p->settled = !moving;
    }
    return pose_of(p, now) != p->pose;
}

void procs_look(struct procs *p, int x, int y)
{
    p->px = x;
    p->py = y;
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < p->count; i++) {
        const struct proc_node *n = &p->n[i];
        int dx = x - n->x, dy = y - n->y, d2 = dx * dx + dy * dy, reach = n->r + 4;
        if (d2 <= reach * reach && d2 < bd) { bd = d2; best = i; }
    }
    p->hover = best;
}

/* Text at (x,y) inside the window, cut to whole characters when it is too wide. */
static void text_in(struct canvas *cv, const struct font *f, int ox, int oy, int w, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    char cut[256];
    snprintf(cut, sizeof cut, "%s", s);
    while (cut[0] && x + font_text_width(f, cut) > w - 2) {
        size_t n = strlen(cut) - 1;
        while (n > 0 && ((unsigned char)cut[n] & 0xC0) == 0x80) n--;
        cut[n] = '\0';
    }
    if (!cut[0] || y < 0 || oy + y + FONT_H > g_clip.y1) return;
    font_text(f, cv, ox + x, oy + y, cut, fg, bg);
}

static void human_kb(long kb, char *buf, size_t cap)
{
    if (kb >= 1024 * 1024) snprintf(buf, cap, "%.1f GB", kb / 1048576.0);
    else if (kb >= 1024) snprintf(buf, cap, "%.1f MB", kb / 1024.0);
    else snprintf(buf, cap, "%ld KB", kb);
}

void procs_draw(struct procs *p, struct canvas *cv, const struct font *f, int ox, int oy, long now)
{
    if (!p->sampled || now - p->sampled >= PROCS_SAMPLE_MS) procs_sample(p, now);
    g_clip = (struct clip){ ox < 0 ? 0 : ox, oy < 0 ? 0 : oy, ox + p->w > cv->w ? cv->w : ox + p->w, oy + p->h > cv->h ? cv->h : oy + p->h };
    canvas_fill(cv, ox, oy, p->w, p->h, C_BG);
    double t = now / 1000.0;
    /* the lines to the parents, with a spark running down the busy ones */
    for (int i = 0; i < p->count; i++) {
        const struct proc_node *n = &p->n[i], *par = n->parent >= 0 ? &p->n[n->parent] : NULL;
        int px = par ? par->x : p->w / 2, py = par ? par->y : LEGEND_H + (p->h - LEGEND_H - STATUS_H) / 2;
        if (!par && n->pid == 1) continue;
        int hot = n->cpu > 0 || n->state == 'R' || i == p->hover || (p->hover >= 0 && n->parent == p->hover);
        line(cv, ox + px, oy + py, ox + n->x, oy + n->y, hot ? C_EDGE_HOT : C_EDGE);
        if (n->cpu > 0 || n->state == 'R') {
            double ph = fmod((double)now / SPARK_MS + i * 0.37, 1.0);
            disc(cv, ox + px + (int)((n->x - px) * ph), oy + py + (int)((n->y - py) * ph), 2, C_SPARK);
        }
    }
    /* the bubbles */
    for (int i = 0; i < p->count; i++) {
        const struct proc_node *n = &p->n[i];
        int r = n->r;
        if (n->state == 'R' || n->cpu > 0) r += (int)lround(1.5 + 1.5 * sin(t * 2 * M_PI / 1.1 + i));
        uint32_t c = n->pid == 1 ? C_CORE : state_color(n->state);
        if (now - n->born < FLASH_MS) ring(cv, ox + n->x, oy + n->y, r + 2 + (int)((now - n->born) / 40), 0, 360, 1, C_FLASH);
        if (i == p->hover) disc(cv, ox + n->x, oy + n->y, r + 3, C_HOVER);
        disc(cv, ox + n->x, oy + n->y, r, c);
        if (n->pid == 1) disc(cv, ox + n->x, oy + n->y, r - 4, C_SLEEP);         /* PID 1 is the OS's heart: a core */
        if (n->cpu > 0) ring(cv, ox + n->x, oy + n->y, r + 2, -90, -90 + 360.0 * (n->cpu > 1000 ? 1000 : n->cpu) / 1000, 2, C_CPU);
    }
    /* names: the big ones, the busy ones, the first ring and the one under the pointer */
    int labels = 0, lx0[32], lx1[32], ly0[32];
    for (int pass = 0; pass < 2 && labels < 32; pass++)             /* the important ones first, then the rest */
        for (int i = 0; i < p->count && labels < 32; i++) {
            const struct proc_node *n = &p->n[i];
            int first = i == p->hover || i == p->hot || n->cpu > 0 || n->state == 'R' || n->pid == 1;
            int want = first || n->depth <= 1 || n->r >= 9;
            if (!want || (pass == 0) != first) continue;
            int tw = font_text_width(f, n->name);
            int lx = n->x + n->r + 4, ly = n->y - FONT_H / 2;
            if (n->pid == 1) { lx = n->x - tw / 2; ly = n->y + n->r + 3; }       /* the centre: named underneath */
            if (lx + tw > p->w - 2) lx = n->x - n->r - 4 - tw;
            if (lx < 2 || ly < LEGEND_H || ly + FONT_H > p->h - STATUS_H) continue;
            int clash = 0;
            for (int k = 0; k < labels; k++) if (lx < lx1[k] + 4 && lx + tw + 4 > lx0[k] && abs(ly - ly0[k]) < FONT_H) clash = 1;
            if (clash) continue;
            font_text(f, cv, ox + lx, oy + ly, n->name, first ? C_TEXT : C_TEXT_DIM, C_BG);
            lx0[labels] = lx; lx1[labels] = lx + tw; ly0[labels] = ly;
            labels++;
        }
    /* the legend above, the summary (or the hovered process) below */
    {
        int x = 6, y = 3;
        static const struct { uint32_t c; const char *s; } key[] = { { C_RUN, "動いている" }, { C_SLEEP, "待っている" }, { C_KERNEL, "カーネル" }, { C_CPU, "輪=CPU" } };
        for (size_t k = 0; k < sizeof key / sizeof key[0]; k++) {
            disc(cv, ox + x + 4, oy + y + FONT_H / 2, 4, key[k].c);
            x += 12;
            font_text(f, cv, ox + x, oy + y, key[k].s, C_TEXT_DIM, C_BG);
            x += font_text_width(f, key[k].s) + 12;
        }
        text_in(cv, f, ox, oy, p->w, p->w - font_text_width(f, "大きさ=メモリ") - 6, y, "大きさ=メモリ", C_TEXT_DIM, C_BG);
    }
    char status[256];
    if (p->hover >= 0) {
        const struct proc_node *n = &p->n[p->hover];
        char mem[32];
        human_kb(n->rss_kb, mem, sizeof mem);
        if (n->state == 'K') snprintf(status, sizeof status, "カーネル ・ %d 本のスレッドが OS の中で働いている", n->kernel);
        else {
            int k = snprintf(status, sizeof status, "%s ・ PID %d ・ %s ・ CPU %d.%d%% ・ %s", n->name, n->pid, state_word(n->state), n->cpu / 10, n->cpu % 10, mem);
            if (n->nchild && k > 0 && (size_t)k < sizeof status) snprintf(status + k, sizeof status - (size_t)k, " ・ 子 %d", n->nchild);
        }
    } else procs_summary(p, status, sizeof status);
    canvas_hline(cv, ox, oy + p->h - STATUS_H, p->w, C_EDGE);
    text_in(cv, f, ox, oy, p->w, 6, p->h - STATUS_H + 3, status, C_TEXT, C_BG);
    p->pose = pose_of(p, now);
}

const char *procs_hot(const struct procs *p) { return p->hot >= 0 ? p->n[p->hot].name : ""; }

const char *procs_summary(const struct procs *p, char *buf, size_t cap)
{
    int user = p->total - p->kernel;
    if (p->hot >= 0 && p->n[p->hot].cpu > 0)
        snprintf(buf, cap, "プロセス %d ・ 動いている %d ・ 忙しい: %s %d.%d%%", user, p->running, p->n[p->hot].name, p->n[p->hot].cpu / 10, p->n[p->hot].cpu % 10);
    else if (p->hot >= 0)
        snprintf(buf, cap, "プロセス %d ・ 動いている %d ・ よく働いた: %s", user, p->running, p->n[p->hot].name);
    else snprintf(buf, cap, "プロセスが見えない (%s が読めない)", p->root);
    return buf;
}

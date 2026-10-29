/* What AlterEgo can draw, and learning more. See drawings.h. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "drawings.h"
#include "store.h"

#define DRAWINGS_FILE "drawings.txt"
#define LEARNED_MAX   64
#define ITEMS_MAX     8

static int has(const char *s, const char *needle) { return strstr(s, needle) != NULL; }

static int has_word(const char *lo, const char *w)
{
    size_t n = strlen(w);
    if (!n) return 0;
    for (const char *p = strstr(lo, w); p; p = strstr(p + 1, w))
        if ((p == lo || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[n])) return 1;
    return 0;
}

static void lower_ascii(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }

/* ------------------------------------------------------------------------ */
/* built in */

static const struct { const char *ja, *ja2, *en, *en2, *spec; } builtin[] = {
    { "丸", "円", "circle", "round", "c 800 500 300" },
    { "線", "直線", "line", NULL, "l 300 500 1300 500" },
    { "四角", "しかく", "square", "rectangle", "p 5 500 250 1100 250 1100 750 500 750 500 250" },
    { "三角", "さんかく", "triangle", NULL, "p 4 800 200 1150 800 450 800 800 200" },
    { "星", "ほし", "star", NULL, "p 6 800 150 1006 783 467 392 1133 392 594 783 800 150" },
    { "ハート", "はーと", "heart", NULL, "p 17 800 420 818 366 913 288 1052 313 1120 440 1052 586 913 712 818 816 800 860 782 816 687 712 548 586 480 440 548 313 687 288 782 366 800 420" },
    { "顔", "かお", "face", "smiley", "c 800 500 300 C 690 420 30 C 910 420 30 p 5 660 600 720 660 800 680 880 660 940 600" },
    { "太陽", "たいよう", "sun", NULL, "k FB8C00 C 800 500 200 s 8 l 800 200 800 100 l 800 800 800 900 l 500 500 400 500 l 1100 500 1200 500 l 588 288 517 217 l 1012 288 1083 217 l 588 712 517 783 l 1012 712 1083 783" },
    { "家", "いえ", "house", "home", "k 8D6E63 P 4 500 500 1100 500 1100 900 500 900 k E53935 P 3 450 500 800 200 1150 500 k 1E88E5 P 4 740 650 860 650 860 900 740 900" },
    { "木", "き", "tree", NULL, "k 8D6E63 P 4 740 600 860 600 860 950 740 950 k 43A047 C 800 450 250" },
    { "花", "はな", "flower", NULL, "k 43A047 s 6 l 800 600 800 950 k F06292 C 800 300 90 C 930 390 90 C 670 390 90 C 720 540 90 C 880 540 90 k FDD835 C 800 430 70" },
    { "山", "やま", "mountain", NULL, "k 7F7F7F P 3 250 900 800 200 1350 900 k FFFFFF P 3 690 350 800 200 910 350" },
    { "猫", "ねこ", "cat", NULL, "c 800 520 280 P 3 580 330 620 120 760 270 P 3 1020 330 980 120 840 270 C 700 470 25 C 900 470 25 P 3 770 580 830 580 800 620 l 500 560 720 600 l 500 640 720 620 l 1100 560 880 600 l 1100 640 880 620" },
    { "魚", "さかな", "fish", NULL, "p 13 1050 500 1010 585 900 647 750 670 600 647 490 585 450 500 490 415 600 353 750 330 900 353 1010 415 1050 500 P 3 1050 500 1250 350 1250 650 C 560 450 20" },
    { "車", "くるま", "car", NULL, "P 4 400 500 1200 500 1200 750 400 750 P 4 550 500 700 350 950 350 1050 500 k 000000 C 600 780 70 C 1000 780 70" },
    { NULL, NULL, NULL, NULL, NULL }
};
#define NBUILTIN 15

/* ------------------------------------------------------------------------ */
/* learned: drawings.txt, one per line: name<TAB>spec */

struct learned { char name[DRAW_NAME]; char spec[DRAW_SPEC]; };

static int load_learned(struct learned *l, int max)
{
    char *all = store_read_all(DRAWINGS_FILE);
    int n = 0;
    if (!all) return 0;
    for (char *p = all; *p && n < max; ) {
        char *e = strchr(p, '\n');
        if (e) *e = '\0';
        char *tab = strchr(p, '\t');
        if (tab && tab > p && tab[1]) {
            *tab = '\0';
            snprintf(l[n].name, sizeof l[n].name, "%s", p);
            snprintf(l[n].spec, sizeof l[n].spec, "%s", tab + 1);
            n++;
        }
        if (!e) break;
        p = e + 1;
    }
    free(all);
    return n;
}

static int save_learned(const struct learned *l, int n)
{
    size_t cap = (size_t)n * (DRAW_NAME + DRAW_SPEC + 2) + 1;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t o = 0;
    for (int i = 0; i < n; i++) o += (size_t)snprintf(buf + o, cap - o, "%s\t%s\n", l[i].name, l[i].spec);
    int rc = store_write(DRAWINGS_FILE, buf);
    free(buf);
    return rc;
}

static void from_builtin(int i, struct drawing *out)
{
    snprintf(out->ja, sizeof out->ja, "%s", builtin[i].ja);
    snprintf(out->en, sizeof out->en, "%s", builtin[i].en);
    snprintf(out->spec, sizeof out->spec, "%s", builtin[i].spec);
    out->learned = 0;
}

static void from_learned(const struct learned *l, struct drawing *out)
{
    snprintf(out->ja, sizeof out->ja, "%s", l->name);
    snprintf(out->en, sizeof out->en, "%s", l->name);
    snprintf(out->spec, sizeof out->spec, "%s", l->spec);
    out->learned = 1;
}

/* The longest name that occurs in the text wins (「雪だるま」 over 「丸」). */
int drawings_find(const char *line, const char *lo, struct drawing *out)
{
    struct learned l[LEARNED_MAX];
    int nl = load_learned(l, LEARNED_MAX), best = -1;
    size_t best_len = 0;
    for (int i = 0; i < nl; i++) {
        char lname[DRAW_NAME];
        snprintf(lname, sizeof lname, "%.63s", l[i].name);
        lower_ascii(lname);
        size_t len = strlen(l[i].name);
        if ((has(line, l[i].name) || has_word(lo, lname)) && len > best_len) { best = NBUILTIN + i; best_len = len; }
    }
    for (int i = 0; builtin[i].ja; i++) {
        size_t len = strlen(builtin[i].ja);
        if ((has(line, builtin[i].ja) || (builtin[i].ja2 && has(line, builtin[i].ja2)) || has_word(lo, builtin[i].en) || (builtin[i].en2 && has_word(lo, builtin[i].en2))) && len > best_len) { best = i; best_len = len; }
    }
    if (best < 0) return -1;
    if (best < NBUILTIN) from_builtin(best, out); else from_learned(&l[best - NBUILTIN], out);
    return best;
}

int drawings_nth(int n, struct drawing *out)
{
    if (n < 0) return 0;
    if (n < NBUILTIN) { from_builtin(n, out); return 1; }
    struct learned l[LEARNED_MAX];
    int nl = load_learned(l, LEARNED_MAX);
    if (n - NBUILTIN >= nl) return 0;
    from_learned(&l[n - NBUILTIN], out);
    return 1;
}

void drawings_list(enum brain_lang lang, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; builtin[i].ja && o + 16 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? (lang == BRAIN_EN ? ", " : "、") : "", lang == BRAIN_EN ? builtin[i].en : builtin[i].ja);
    struct learned l[LEARNED_MAX];
    int nl = load_learned(l, LEARNED_MAX);
    for (int i = 0; i < nl && o + 16 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%s", lang == BRAIN_EN ? ", " : "、", l[i].name);
}

static int remember_drawing(const char *name, const char *spec)
{
    struct learned l[LEARNED_MAX];
    int n = load_learned(l, LEARNED_MAX), at = -1;
    for (int i = 0; i < n; i++) if (strcmp(l[i].name, name) == 0) at = i;
    if (at < 0) {
        if (n == LEARNED_MAX) { memmove(l, l + 1, (LEARNED_MAX - 1) * sizeof l[0]); n--; }
        at = n++;
    }
    snprintf(l[at].name, sizeof l[at].name, "%s", name);
    snprintf(l[at].spec, sizeof l[at].spec, "%s", spec);
    return save_learned(l, n);
}

int drawings_forget(const char *name)
{
    struct learned l[LEARNED_MAX];
    int n = load_learned(l, LEARNED_MAX), found = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(l[i].name, name) != 0) continue;
        memmove(l + i, l + i + 1, (size_t)(n - i - 1) * sizeof l[0]);
        n--; i--; found = 1;
    }
    if (found) save_learned(l, n);
    return found;
}

/* ------------------------------------------------------------------------ */
/* from a picture */

int drawings_learn_picture(const char *name, const char *path, enum brain_lang lang, char *err, size_t cap)
{
    int en = lang == BRAIN_EN;
    const char *dir = store_open();
    if (!dir) { snprintf(err, cap, en ? "I have nowhere to keep it (no memory disk)." : "覚えておく場所がない(記憶ディスクが無い)。"); return -1; }
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, cap, en ? "I cannot read that picture." : "その絵が読めない。"); return -1; }
    char magic[3] = "";
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 || strcmp(magic, "P6") != 0 || w <= 0 || h <= 0 || w > 8192 || h > 8192) { fclose(f); snprintf(err, cap, en ? "That is not a picture I understand." : "知らない形式の絵だ。"); return -1; }
    fgetc(f);
    size_t n = (size_t)w * h * 3;
    unsigned char *px = malloc(n);
    if (!px || fread(px, 1, n, f) != n) { free(px); fclose(f); snprintf(err, cap, en ? "That picture is broken." : "その絵は壊れている。"); return -1; }
    fclose(f);
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const unsigned char *p = px + ((size_t)y * w + x) * 3;
        if (p[0] > 235 && p[1] > 235 && p[2] > 235) continue;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (x1 < 0) { free(px); snprintf(err, cap, en ? "That sheet is empty." : "その絵は真っ白だ。"); return -1; }
    /* keep a copy under drawings/, named after the drawing */
    char ddir[600], dest[700];
    snprintf(ddir, sizeof ddir, "%s/drawings", dir);
    mkdir(ddir, 0755);
    char safe[DRAW_NAME * 3];
    size_t o = 0;
    for (const char *p = name; *p && o + 4 < sizeof safe; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == ' ' || c == '\t' || c < 0x20) safe[o++] = '_'; else safe[o++] = (char)c;
    }
    safe[o] = '\0';
    snprintf(dest, sizeof dest, "%.590s/%.90s.ppm", ddir, safe);
    FILE *d = fopen(dest, "wb");
    if (!d) { free(px); snprintf(err, cap, en ? "I cannot write it down." : "書き留められない。"); return -1; }
    fprintf(d, "P6\n%d %d\n255\n", w, h);
    int ok = fwrite(px, 1, n, d) == n;
    ok = fclose(d) == 0 && ok;
    free(px);
    if (!ok) { snprintf(err, cap, en ? "I cannot write it down." : "書き留められない。"); return -1; }
    /* the box the ink takes, in sheet units (1/1000 of the height) */
    char spec[DRAW_SPEC];
    snprintf(spec, sizeof spec, "i %s %ld %ld %ld %ld", dest, (long)x0 * 1000 / h, (long)y0 * 1000 / h, (long)(x1 + 1) * 1000 / h, (long)(y1 + 1) * 1000 / h);
    if (remember_drawing(name, spec) != 0) { snprintf(err, cap, en ? "I cannot write it down." : "書き留められない。"); return -1; }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* from words: placing known drawings */

struct box { long x0, y0, x1, y1; };

/* Walk a spec, calling fn for each coordinate pair (and radius), to find
 * its box or to move it. Returns 0 on a malformed spec. */
static int spec_box(const char *s, struct box *b)
{
    b->x0 = b->y0 = 1000000; b->x1 = b->y1 = -1000000;
    int any = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        char cmd = *s++;
        char *end;
        long v[4];
        if (cmd == 'k') { strtoul(s, &end, 16); s = end; continue; }
        if (cmd == 'e') continue;
        if (cmd == 's') { strtol(s, &end, 10); s = end; continue; }
        if (cmd == 'i') {
            while (*s == ' ') s++;
            s += strcspn(s, " ");
            for (int i = 0; i < 4; i++) { v[i] = strtol(s, &end, 10); if (end == s) return 0; s = end; }
            if (v[0] < b->x0) b->x0 = v[0];
            if (v[2] > b->x1) b->x1 = v[2];
            if (v[1] < b->y0) b->y0 = v[1];
            if (v[3] > b->y1) b->y1 = v[3];
            any = 1;
            continue;
        }
        int pairs = cmd == 'l' ? 2 : (cmd == 'c' || cmd == 'C') ? 1 : (cmd == 'p' || cmd == 'P') ? -1 : 0;
        if (!pairs) return 0;
        if (pairs < 0) { pairs = (int)strtol(s, &end, 10); if (end == s || pairs < 1 || pairs > 30) return 0; s = end; }
        for (int i = 0; i < pairs; i++) {
            long x = strtol(s, &end, 10); if (end == s) return 0; s = end;
            long y = strtol(s, &end, 10); if (end == s) return 0; s = end;
            long r = 0;
            if (cmd == 'c' || cmd == 'C') { r = strtol(s, &end, 10); if (end == s) return 0; s = end; }
            if (x - r < b->x0) b->x0 = x - r;
            if (x + r > b->x1) b->x1 = x + r;
            if (y - r < b->y0) b->y0 = y - r;
            if (y + r > b->y1) b->y1 = y + r;
            any = 1;
        }
    }
    return any;
}

/* Copy the spec with every coordinate mapped by x' = dx + x * scale / 1000. */
static size_t spec_move(const char *s, long scale, long dx, long dy, char *out, size_t cap)
{
    size_t o = 0;
#define PUT(...) do { if (o < cap) o += (size_t)snprintf(out + o, cap - o, __VA_ARGS__); } while (0)
    while (*s && o + 16 < cap) {
        while (*s == ' ') s++;
        if (!*s) break;
        char cmd = *s++;
        char *end;
        if (cmd == 'k') { unsigned long c = strtoul(s, &end, 16); s = end; PUT("k %06lX ", c & 0xFFFFFF); continue; }
        if (cmd == 'e') continue;                                     /* never empty the sheet inside a composition */
        if (cmd == 's') { long v = strtol(s, &end, 10); s = end; PUT("s %ld ", v * scale / 1000 < 1 ? 1 : v * scale / 1000); continue; }
        if (cmd == 'i') {
            while (*s == ' ') s++;
            size_t pl = strcspn(s, " ");
            PUT("i %.*s", (int)pl, s);
            s += pl;
            for (int i = 0; i < 4; i++) { long v = strtol(s, &end, 10); if (end == s) return o; s = end; PUT(" %ld", (i % 2 == 0 ? dx : dy) + v * scale / 1000); }
            PUT(" ");
            continue;
        }
        int pairs = cmd == 'l' ? 2 : (cmd == 'c' || cmd == 'C') ? 1 : (cmd == 'p' || cmd == 'P') ? -1 : 0;
        if (!pairs) return o;
        if (pairs < 0) { pairs = (int)strtol(s, &end, 10); if (end == s) return o; s = end; PUT("%c %d", cmd, pairs); }
        else PUT("%c", cmd);
        for (int i = 0; i < pairs; i++) {
            long x = strtol(s, &end, 10); if (end == s) return o; s = end;
            long y = strtol(s, &end, 10); if (end == s) return o; s = end;
            PUT(" %ld %ld", dx + x * scale / 1000, dy + y * scale / 1000);
            if (cmd == 'c' || cmd == 'C') { long r = strtol(s, &end, 10); if (end == s) return o; s = end; PUT(" %ld", r * scale / 1000 < 1 ? 1 : r * scale / 1000); }
        }
        PUT(" ");
    }
#undef PUT
    return o;
}

struct item {
    struct drawing d;
    long scale;                 /* per mille */
    const char *color;          /* hex or NULL */
    struct box b;               /* placed box, in sheet units */
};

enum { R_NONE = 0, R_ABOVE, R_BELOW, R_LEFT, R_RIGHT, R_INSIDE };

static const struct { const char *ja, *en, *hex; } colors[] = {
    { "赤", "red", "E53935" }, { "青", "blue", "1E88E5" }, { "緑", "green", "43A047" }, { "黄色", "yellow", "FDD835" }, { "黄", "yellow", "FDD835" },
    { "黒", "black", "000000" }, { "紫", "purple", "8E24AA" }, { "ピンク", "pink", "F06292" }, { "桃色", "pink", "F06292" }, { "水色", "cyan", "00ACC1" },
    { "オレンジ", "orange", "FB8C00" }, { "橙", "orange", "FB8C00" }, { "茶色", "brown", "8D6E63" }, { "灰色", "gray", "7F7F7F" }, { NULL, NULL, NULL }
};

/* One thing in a description: 「大きい赤い丸」 / "a big red circle". */
static int parse_item(const char *text, struct item *it)
{
    char lo[256];
    snprintf(lo, sizeof lo, "%s", text);
    lower_ascii(lo);
    memset(it, 0, sizeof *it);
    it->scale = 1000;
    if (has(text, "大きい") || has(text, "大きな") || has(text, "でかい") || has(text, "おおきい") || has_word(lo, "big") || has_word(lo, "large")) it->scale = 1500;
    if (has(text, "小さい") || has(text, "小さな") || has(text, "ちいさい") || has(text, "ちっちゃい") || has_word(lo, "small") || has_word(lo, "little") || has_word(lo, "tiny")) it->scale = 500;
    for (int i = 0; colors[i].ja; i++) if (has(text, colors[i].ja) || has_word(lo, colors[i].en)) { it->color = colors[i].hex; break; }
    return drawings_find(text, lo, &it->d) >= 0;
}

static void place(struct item *it, int rel, const struct item *ref)
{
    struct box nb;
    if (!spec_box(it->d.spec, &nb)) { nb.x0 = 500; nb.y0 = 200; nb.x1 = 1100; nb.y1 = 800; }
    long cx = (nb.x0 + nb.x1) / 2, cy = (nb.y0 + nb.y1) / 2;
    long hx = (nb.x1 - nb.x0) * it->scale / 2000, hy = (nb.y1 - nb.y0) * it->scale / 2000;
    long tx = 800, ty = 500;
    const long gap = 30;
    if (ref) {
        long rcx = (ref->b.x0 + ref->b.x1) / 2, rcy = (ref->b.y0 + ref->b.y1) / 2;
        switch (rel) {
        case R_ABOVE:  tx = rcx; ty = ref->b.y0 - gap - hy; break;
        case R_BELOW:  tx = rcx; ty = ref->b.y1 + gap + hy; break;
        case R_LEFT:   tx = ref->b.x0 - gap - hx; ty = rcy; break;
        case R_RIGHT:  tx = ref->b.x1 + gap + hx; ty = rcy; break;
        case R_INSIDE: tx = rcx; ty = rcy; break;
        default:       tx = ref->b.x1 + gap + hx; ty = rcy; break;        /* next to the previous one */
        }
    }
    it->b.x0 = tx - hx; it->b.x1 = tx + hx; it->b.y0 = ty - hy; it->b.y1 = ty + hy;
    (void)cx; (void)cy;
}

/* Place the items, fit them on the sheet and join their specs. */
static int compose(struct item *items, int n, char *spec, size_t cap)
{
    struct box all = { 1000000, 1000000, -1000000, -1000000 };
    for (int i = 0; i < n; i++) {
        if (items[i].b.x0 < all.x0) all.x0 = items[i].b.x0;
        if (items[i].b.y0 < all.y0) all.y0 = items[i].b.y0;
        if (items[i].b.x1 > all.x1) all.x1 = items[i].b.x1;
        if (items[i].b.y1 > all.y1) all.y1 = items[i].b.y1;
    }
    long w = all.x1 - all.x0, h = all.y1 - all.y0;
    long fit = 1000;                                                   /* shrink to the sheet (about 1500 x 900) */
    if (w > 1500) fit = 1500 * 1000 / w;
    if (h > 900 && 900 * 1000 / h < fit) fit = 900 * 1000 / h;
    long acx = (all.x0 + all.x1) / 2, acy = (all.y0 + all.y1) / 2;
    size_t o = 0;
    spec[0] = '\0';
    for (int i = 0; i < n; i++) {
        struct item *it = &items[i];
        struct box nb;
        if (!spec_box(it->d.spec, &nb)) { nb.x0 = 500; nb.y0 = 200; nb.x1 = 1100; nb.y1 = 800; }
        long scx = (nb.x0 + nb.x1) / 2, scy = (nb.y0 + nb.y1) / 2;
        long tcx = 800 + ((it->b.x0 + it->b.x1) / 2 - acx) * fit / 1000, tcy = 500 + ((it->b.y0 + it->b.y1) / 2 - acy) * fit / 1000;
        long scale = it->scale * fit / 1000;
        if (it->color && o + 12 < cap) o += (size_t)snprintf(spec + o, cap - o, "k %s ", it->color);
        else if (o + 12 < cap) o += (size_t)snprintf(spec + o, cap - o, "k 000000 ");
        o += spec_move(it->d.spec, scale, tcx - scx * scale / 1000, tcy - scy * scale / 1000, spec + o, cap - o);
        if (o + 32 >= cap) return -1;
    }
    return 0;
}

static const struct { const char *word; int rel; } rel_ja[] = {
    { "の上に", R_ABOVE }, { "のうえに", R_ABOVE }, { "の下に", R_BELOW }, { "のしたに", R_BELOW }, { "の左に", R_LEFT }, { "のひだりに", R_LEFT },
    { "の右に", R_RIGHT }, { "のみぎに", R_RIGHT }, { "の中に", R_INSIDE }, { "のなかに", R_INSIDE }, { "の隣に", R_RIGHT }, { "のとなりに", R_RIGHT }, { "の横に", R_RIGHT },
    { NULL, 0 }
};
static const struct { const char *word; int rel; } rel_en[] = {
    { " above ", R_ABOVE }, { " on top of ", R_ABOVE }, { " over ", R_ABOVE }, { " on ", R_ABOVE }, { " below ", R_BELOW }, { " under ", R_BELOW }, { " beneath ", R_BELOW },
    { " left of ", R_LEFT }, { " to the left of ", R_LEFT }, { " right of ", R_RIGHT }, { " to the right of ", R_RIGHT }, { " next to ", R_RIGHT }, { " beside ", R_RIGHT },
    { " inside ", R_INSIDE }, { " in ", R_INSIDE }, { NULL, 0 }
};

static int is_previous(const char *s)
{
    char lo[128];
    snprintf(lo, sizeof lo, "%s", s);
    lower_ascii(lo);
    return has(s, "その") || has(s, "それ") || strcmp(s, "") == 0 || has_word(lo, "it") || has_word(lo, "that") || has_word(lo, "them");
}

int drawings_learn_words(const char *name, const char *desc, enum brain_lang lang, char *err, size_t cap)
{
    int en = lang == BRAIN_EN;
    struct item items[ITEMS_MAX];
    int n = 0;
    char text[1024];
    snprintf(text, sizeof text, "%s", desc);
    /* clauses: 「、」 「と」 「。」 / "," "and" "." */
    char *clauses[ITEMS_MAX * 2];
    int nc = 0;
    for (char *p = text; *p && nc < ITEMS_MAX * 2; ) {
        clauses[nc++] = p;
        char *cut = NULL;
        static const char *const seps[] = { "、", "。", "，", ",", " and ", "と", ". ", NULL };
        for (int i = 0; seps[i]; i++) { char *q = strstr(p, seps[i]); if (q && (!cut || q < cut)) cut = q; }
        if (!cut) break;
        size_t sl = 0;
        for (int i = 0; seps[i]; i++) if (strncmp(cut, seps[i], strlen(seps[i])) == 0 && strlen(seps[i]) > sl) sl = strlen(seps[i]);
        memset(cut, '\0', sl);
        p = cut + sl;
    }
    for (int c = 0; c < nc; c++) {
        char *cl = clauses[c];
        while (*cl == ' ') cl++;
        if (!*cl) continue;
        int rel = R_NONE;
        char *a = NULL, *b = cl;
        for (int i = 0; rel_ja[i].word && !a; i++) { char *q = strstr(cl, rel_ja[i].word); if (q) { *q = '\0'; a = cl; b = q + strlen(rel_ja[i].word); rel = rel_ja[i].rel; } }
        if (!a) {
            char lo[1024];
            snprintf(lo, sizeof lo, "%s", cl);
            lower_ascii(lo);
            for (int i = 0; rel_en[i].word && !a; i++) {
                char *q = strstr(lo, rel_en[i].word);
                if (q) { size_t at = (size_t)(q - lo); cl[at] = '\0'; b = cl; a = cl + at + strlen(rel_en[i].word); rel = rel_en[i].rel; }   /* "B above A" */
            }
        }
        const struct item *ref = NULL;
        if (a) {
            if (is_previous(a)) ref = n ? &items[n - 1] : NULL;
            else {
                int found = -1;
                for (int i = 0; i < n; i++) if (has(a, items[i].d.ja) || (items[i].d.en[0] && has(a, items[i].d.en))) found = i;
                if (found >= 0) ref = &items[found];
                else {
                    if (n >= ITEMS_MAX) break;
                    if (!parse_item(a, &items[n])) { snprintf(err, cap, en ? "I do not know how to draw \"%s\" yet." : "「%s」の描き方をまだ知らない。先にそれを教えて。", a); return -1; }
                    place(&items[n], R_NONE, n ? &items[n - 1] : NULL);
                    ref = &items[n];
                    n++;
                }
            }
        } else if (n) ref = &items[n - 1];
        if (n >= ITEMS_MAX) break;
        while (*b == ' ') b++;
        if (!*b) continue;
        if (!parse_item(b, &items[n])) { snprintf(err, cap, en ? "I do not know how to draw \"%s\" yet." : "「%s」の描き方をまだ知らない。先にそれを教えて。", b); return -1; }
        if (rel == R_INSIDE && items[n].scale == 1000) items[n].scale = 450;
        place(&items[n], ref ? rel : R_NONE, ref);
        n++;
    }
    if (n == 0) { snprintf(err, cap, en ? "Tell me what it is made of, like \"a big circle, a small circle above it\"." : "何でできているか教えて。「大きい丸の上に小さい丸」のように。"); return -1; }
    char spec[DRAW_SPEC];
    if (compose(items, n, spec, sizeof spec) != 0) { snprintf(err, cap, en ? "That is too much for one drawing." : "それは一つの絵には多すぎる。"); return -1; }
    if (!store_open()) { snprintf(err, cap, en ? "I have nowhere to keep it (no memory disk)." : "覚えておく場所がない(記憶ディスクが無い)。"); return -1; }
    if (remember_drawing(name, spec) != 0) { snprintf(err, cap, en ? "I cannot write it down." : "書き留められない。"); return -1; }
    return 0;
}

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dict.h"
#include "../agent/utf8.h"

static uint32_t hash_str(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static int key_at(const struct dict *d, uint32_t off, const char *key, size_t n)
{
    const char *p = d->data + off;
    return strncmp(p, key, n) == 0 && p[n] == '\t';
}

static void insert(struct dict *d, uint32_t off, const char *key, size_t n)
{
    size_t i = hash_str(key, n) & (d->cap - 1);
    while (d->idx[i]) i = (i + 1) & (d->cap - 1);
    d->idx[i] = off + 1;
}

int dict_load(struct dict *d, const char *path)
{
    memset(d, 0, sizeof *d);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) < 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 64L * 1024 * 1024) { fclose(f); return -1; }
    rewind(f);
    d->data = malloc((size_t)sz + 1);
    if (!d->data || fread(d->data, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d->data); d->data = NULL; return -1; }
    fclose(f);
    d->len = (size_t)sz;
    d->data[sz] = '\0';
    int lines = 0;
    for (size_t i = 0; i < d->len; i++) if (d->data[i] == '\n') lines++;
    d->cap = 16;
    while (d->cap < (size_t)lines * 2 + 16) d->cap *= 2;
    d->idx = calloc(d->cap, sizeof *d->idx);
    if (!d->idx) { free(d->data); d->data = NULL; return -1; }
    size_t p = 0;
    while (p < d->len) {
        char *nl = memchr(d->data + p, '\n', d->len - p);
        size_t end = nl ? (size_t)(nl - d->data) : d->len;
        if (nl) *nl = '\0';
        char *tab = memchr(d->data + p, '\t', end - p);
        if (tab && d->data[p] != '#' && tab > d->data + p) { insert(d, (uint32_t)p, d->data + p, (size_t)(tab - (d->data + p))); d->n++; }
        p = end + 1;
    }
    return 0;
}

void dict_free(struct dict *d)
{
    free(d->data);
    free(d->idx);
    memset(d, 0, sizeof *d);
}

const char *dict_lookup(const struct dict *d, const char *key)
{
    if (!d->idx || !*key) return NULL;
    size_t n = strlen(key);
    size_t i = hash_str(key, n) & (d->cap - 1);
    while (d->idx[i]) {
        uint32_t off = d->idx[i] - 1;
        if (key_at(d, off, key, n)) return d->data + off + n + 1;
        i = (i + 1) & (d->cap - 1);
    }
    return NULL;
}

char dict_okuri_letter(uint32_t cp)
{
    /* U+3041 ぁ .. U+3096 ゖ, one letter per code point */
    static const char letters[] =
        "aaiiuueeoo"            /* ぁあぃいぅうぇえぉお */
        "kgkgkgkgkg"            /* かがきぎくぐけげこご */
        "szszszszsz"            /* さざしじすずせぜそぞ */
        "tdtdttdtdtd"           /* ただちぢっつづてでとど */
        "nnnnn"                 /* なにぬねの */
        "hbphbphbphbphbp"       /* はばぱひびぴふぶぷへべぺほぼぽ */
        "mmmmm"                 /* まみむめも */
        "yyyyyy"                /* ゃやゅゆょよ */
        "rrrrr"                 /* らりるれろ */
        "wwwwwn"                /* ゎわゐゑをん */
        "v"                     /* ゔ */
        "kk";                   /* ゕゖ */
    if (cp < 0x3041 || cp > 0x3096) return 0;
    return letters[cp - 0x3041];
}

static int split_cands(const char *list, const char *suffix, char cands[][DICT_CAND], int n, int max)
{
    while (*list && n < max) {
        const char *sl = strchr(list, '/');
        size_t len = sl ? (size_t)(sl - list) : strlen(list);
        if (len > 0 && len + strlen(suffix) < DICT_CAND) {
            memcpy(cands[n], list, len);
            cands[n][len] = '\0';
            strcat(cands[n], suffix);
            int dup = 0;
            for (int i = 0; i < n; i++) if (strcmp(cands[i], cands[n]) == 0) dup = 1;
            if (!dup) n++;
        }
        if (!sl) break;
        list = sl + 1;
    }
    return n;
}

/* Byte offset of the i-th code point boundary from the end: positions of each code point start. */
static int cp_starts(const char *s, size_t *pos, int max)
{
    int n = 0;
    size_t i = 0, len = strlen(s);
    while (i < len && n < max) {
        pos[n++] = i;
        uint32_t cp;
        size_t k = utf8_decode(s + i, &cp);
        if (!k) break;
        i += k;
    }
    return n;
}

static void to_katakana(const char *hira, char *out, size_t cap)
{
    size_t o = 0;
    uint32_t cp;
    size_t k;
    while ((k = utf8_decode(hira, &cp)) > 0) {
        if (cp >= 0x3041 && cp <= 0x3096) cp += 0x60;
        char u[4];
        size_t e = utf8_encode(cp, u);
        if (o + e >= cap) break;
        memcpy(out + o, u, e);
        o += e;
        hira += k;
    }
    out[o] = '\0';
}

int dict_convert(const struct dict *d, const char *hira, char cands[][DICT_CAND], int max, size_t *seg)
{
    size_t len = strlen(hira);
    size_t pos[128];
    int ncp = cp_starts(hira, pos, 128);
    int n = 0;
    *seg = len;
    const char *list;

    if (ncp > 0 && (list = dict_lookup(d, hira)) != NULL) {
        n = split_cands(list, "", cands, n, max - 2);
    } else if (ncp > 1) {
        /* okuri-ari (stem + first letter of the inflection, covering the whole
         * input) first, longest stem; else the longest okuri-nasi prefix, whose
         * rest stays hiragana for the next conversion. */
        int best_i = -1, best_okuri = 0;
        const char *best_list = NULL;
        /* the inflection part must be hiragana only (punctuation etc. ends
         * the word, so 「かんじ、」 goes the okuri-nasi way) */
        int hira_from = ncp;
        for (int i = ncp - 1; i >= 0; i--) {
            uint32_t cp;
            utf8_decode(hira + pos[i], &cp);
            if (cp >= 0x3041 && cp <= 0x3096) hira_from = i; else break;
        }
        for (int i = ncp - 1; i >= 1 && i >= hira_from && !best_list; i--) {
            char key[128];
            size_t stem = pos[i];
            if (stem + 2 >= sizeof key) continue;
            uint32_t cp;
            utf8_decode(hira + stem, &cp);
            char letter = dict_okuri_letter(cp);
            if (!letter) continue;
            memcpy(key, hira, stem);
            key[stem] = letter;
            key[stem + 1] = '\0';
            list = dict_lookup(d, key);
            if (list) { best_i = i; best_okuri = 1; best_list = list; }
        }
        for (int i = ncp - 1; i >= 1 && !best_list; i--) {
            char key[128];
            size_t stem = pos[i];
            if (stem + 1 >= sizeof key) continue;
            memcpy(key, hira, stem);
            key[stem] = '\0';
            list = dict_lookup(d, key);
            if (list) { best_i = i; best_okuri = 0; best_list = list; }
        }
        if (best_list) {
            if (best_okuri) { n = split_cands(best_list, hira + pos[best_i], cands, n, max - 2); *seg = len; }
            else { n = split_cands(best_list, "", cands, n, max - 2); *seg = pos[best_i]; }
        }
    }
    /* the hiragana itself and its katakana, for the same part */
    char part[DICT_CAND];
    size_t plen = *seg < sizeof part - 1 ? *seg : sizeof part - 1;
    memcpy(part, hira, plen);
    part[plen] = '\0';
    if (n < max) { snprintf(cands[n], DICT_CAND, "%s", part); n++; }
    if (n < max) { to_katakana(part, cands[n], DICT_CAND); if (strcmp(cands[n], part) != 0) n++; }
    return n;
}

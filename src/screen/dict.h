#ifndef ALTEREGO_DICT_H
#define ALTEREGO_DICT_H

#include <stddef.h>
#include <stdint.h>

/* Kana-to-kanji dictionary, from an SKK dictionary converted by
 * scripts/mkdict.py: one entry per line, "reading<TAB>cand1/cand2/...".
 * Okuri-ari readings end in one romaji letter for the inflection
 * ("たべr" → 食べ, used as 食べ + る). The whole file stays in memory with a
 * hash of its lines. */

#define DICT_CAND 96              /* bytes per candidate */

struct dict {
    char *data;
    size_t len;
    uint32_t *idx;                /* hash table of line offsets + 1; 0 = empty */
    size_t cap;
    int n;
};

int  dict_load(struct dict *d, const char *path);            /* 0 or -1 */
void dict_free(struct dict *d);
/* The candidates for exactly `key` ("c1/c2/..."), or NULL. */
const char *dict_lookup(const struct dict *d, const char *key);
/* Convert the hiragana `hira`: up to `max` candidates for its first part,
 * each already followed by any okurigana; *seg = how many bytes of `hira`
 * they replace (the rest stays hiragana). Always ends with the hiragana
 * itself and its katakana, so there is at least one. Returns the count. */
int dict_convert(const struct dict *d, const char *hira, char cands[][DICT_CAND], int max, size_t *seg);
/* The romaji letter an okurigana starting with `cp` gives an SKK key, or 0. */
char dict_okuri_letter(uint32_t cp);

#endif

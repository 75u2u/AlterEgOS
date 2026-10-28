#ifndef ALTEREGO_IME_H
#define ALTEREGO_IME_H

#include <stddef.h>
#include "dict.h"
#include "kana.h"

/* Japanese input on top of romaji-to-kana: what is typed collects as a
 * hiragana preedit under the cursor instead of going to the program; Space
 * converts it with the dictionary (again: the next candidate; a digit picks
 * one), Enter commits what is shown, Backspace takes a kana back or cancels
 * the conversion, Escape drops it. When the conversion covered only the
 * front of the preedit, the rest stays and can be converted next. Without a
 * dictionary, Space still offers the hiragana and katakana forms. */

#define IME_PRE   256
#define IME_CANDS 20

struct ime {
    struct kana kana;
    const struct dict *dict;        /* may be NULL */
    char pre[IME_PRE];              /* hiragana not yet committed */
    int converting;
    char cands[IME_CANDS][DICT_CAND];
    int ncands, cur;
    size_t seg;                     /* bytes of pre the candidates replace */
};

void   ime_init(struct ime *m, const struct dict *d);
/* One key as the terminal would receive it (ASCII, "\r", "\x7f", "\x1b" or
 * an escape sequence). Bytes to send to the program go to `out`; returns
 * their count. */
size_t ime_key(struct ime *m, const char *text, char *out, size_t cap);
/* Commit everything (mode switch); bytes to send. */
size_t ime_flush(struct ime *m, char *out, size_t cap);
int    ime_active(const struct ime *m);                 /* something is shown at the cursor */
/* What is shown at the cursor: the chosen candidate (its length in *conv
 * bytes) followed by the rest of the preedit and the pending romaji. */
void   ime_shown(const struct ime *m, char *buf, size_t cap, size_t *conv);
/* The candidate list for a popup ("1 漢字  2 幹事 ..."), "" when not converting. */
void   ime_popup(const struct ime *m, char *buf, size_t cap, int *cur_index);

#endif

#ifndef ALTEREGO_IME_H
#define ALTEREGO_IME_H

#include <stddef.h>
#include "dict.h"
#include "kana.h"

/* Japanese input on top of romaji-to-kana: what is typed collects as a
 * hiragana preedit under the cursor instead of going to the program.
 *
 * Space converts the whole preedit at once: it is cut into 文節 (a
 * dictionary word, with its okurigana or the particles and endings that
 * follow it: 今日は / いい / 天気ですね) by a search for the cheapest cut
 * (fewest segments, longest words, words the user chose before first), and
 * every segment gets its own candidates. Space again, or a digit, changes
 * the candidate of the current segment; ←/→ (or Tab) move between the
 * segments; Shift+←/→ shrink or stretch the current segment by one kana
 * and re-cut the rest; Enter commits everything shown; Backspace goes
 * back to the hiragana; Escape drops it.
 *
 * What the user chooses is learned: each segment's reading → word, and the
 * whole phrase, go into a user dictionary (ime.txt in the state directory)
 * and come first the next time. Without a dictionary, Space still offers
 * the hiragana and katakana forms. */

#define IME_PRE   256
#define IME_CANDS 20
#define IME_SEGS  24
#define IME_LEARN 4096

struct ime_seg {
    size_t off, len;                /* bytes of the preedit this segment reads */
    char cands[IME_CANDS][DICT_CAND];
    size_t sufl[IME_CANDS];         /* bytes of particles / okurigana at the end of each candidate */
    int ncands, cur;
};

struct ime_learned { char reading[64]; char word[DICT_CAND]; int count; };

struct ime {
    struct kana kana;
    const struct dict *dict;        /* may be NULL */
    char pre[IME_PRE];              /* hiragana not yet committed */
    int converting;
    struct ime_seg segs[IME_SEGS];
    int nsegs, seg;                 /* segments and the current one */
    struct ime_learned *learned;    /* the user dictionary, newest last */
    int nlearned;
    char learn_path[512];           /* "" = not kept */
};

void   ime_init(struct ime *m, const struct dict *d);
/* Keep what the user chooses in this file (loaded now, rewritten at each commit). */
void   ime_set_learning(struct ime *m, const char *path);
void   ime_free(struct ime *m);
/* One key as the terminal would receive it (ASCII, "\r", "\x7f", "\x1b" or
 * an escape sequence). Bytes to send to the program go to `out`; returns
 * their count. */
size_t ime_key(struct ime *m, const char *text, char *out, size_t cap);
/* Commit everything (mode switch); bytes to send. */
size_t ime_flush(struct ime *m, char *out, size_t cap);
int    ime_active(const struct ime *m);                 /* something is shown at the cursor */
/* What is shown at the cursor: the chosen candidates of all segments, or the
 * preedit and the pending romaji; *sel_off and *sel_len = bytes of the current
 * segment's candidate (0 when not converting). */
void   ime_shown(const struct ime *m, char *buf, size_t cap, size_t *sel_off, size_t *sel_len);
/* The candidate list of the current segment for a popup ("1 漢字  2 幹事 ..."), "" when not converting. */
void   ime_popup(const struct ime *m, char *buf, size_t cap, int *cur_index);
/* How the preedit was cut, for tests: the segments joined with bars (今日は, いい, 天気ですね). */
void   ime_cut(const struct ime *m, char *buf, size_t cap);
/* Cut `hira` into segments and give the first candidate of each, joined ("今日はいい天気ですね"). */
void   ime_convert_text(struct ime *m, const char *hira, char *out, size_t cap);

#endif

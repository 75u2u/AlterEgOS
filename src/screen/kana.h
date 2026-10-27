#ifndef ALTEREGO_KANA_H
#define ALTEREGO_KANA_H

#include <stddef.h>

/* Romaji to kana, letter by letter, without a dictionary: "ka" becomes か,
 * "kya" きゃ, "nn" ん, a doubled consonant っ; typed in capitals the same
 * chunk becomes katakana (KA → カ). Kanji conversion needs a dictionary
 * the OS does not carry yet, so hiragana is where it stops. Letters that
 * can never start a kana (q, x alone, ...) pass through unchanged. */

#define KANA_PEND 8

struct kana {
    char pend[KANA_PEND];       /* romaji typed so far, not yet a kana (lower-case) */
    int  n;
    int  upper;                 /* the chunk was typed in capitals: katakana */
};

void kana_reset(struct kana *k);
/* Feed one ASCII character; UTF-8 to send is appended to `out`, returns its length. */
size_t kana_feed(struct kana *k, int ch, char *out, size_t cap);
/* End of input (Enter, mode switch): emit what is pending ("n" → ん, the rest as typed). */
size_t kana_flush(struct kana *k, char *out, size_t cap);
/* Remove the last pending letter; returns 1 if there was one. */
int    kana_backspace(struct kana *k);

#endif

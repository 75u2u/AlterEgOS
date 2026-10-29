#ifndef ALTEREGO_UTF8_H
#define ALTEREGO_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Just enough UTF-8 to treat Japanese text as a sequence of code points and
 * to tell kanji, kana and latin apart (the "word" boundaries the brain uses
 * without a dictionary). */

size_t   utf8_decode(const char *s, uint32_t *cp);   /* bytes consumed, 0 at end */
size_t   utf8_encode(uint32_t cp, char out[4]);
size_t   utf8_count(const char *s);                 /* code points */

enum ctype { CT_OTHER, CT_SPACE, CT_PUNCT, CT_HIRAGANA, CT_KATAKANA, CT_KANJI, CT_LATIN, CT_DIGIT };
enum ctype utf8_class(uint32_t cp);
int      utf8_wide(uint32_t cp);                   /* takes two terminal cells (CJK, full-width forms) */

#endif

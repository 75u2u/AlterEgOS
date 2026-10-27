#include "utf8.h"

size_t utf8_decode(const char *s, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] == 0) { *cp = 0; return 0; }
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); return 4;
    }
    *cp = 0xFFFD;        /* malformed byte: replacement character, step one byte */
    return 1;
}

size_t utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

size_t utf8_count(const char *s)
{
    size_t n = 0;
    uint32_t cp;
    size_t k;
    while ((k = utf8_decode(s, &cp)) > 0) { s += k; n++; }
    return n;
}

enum ctype utf8_class(uint32_t cp)
{
    if (cp == ' ' || cp == '\t' || cp == 0x3000) return CT_SPACE;
    if (cp < 0x80) {
        if (cp >= '0' && cp <= '9') return CT_DIGIT;
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return CT_LATIN;
        if (cp == '\'') return CT_LATIN;                  /* don't, I'm: one word */
        return CT_PUNCT;
    }
    if (cp >= 0x3041 && cp <= 0x309F) return CT_HIRAGANA;
    if ((cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) return CT_KATAKANA;
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || cp == 0x3005) return CT_KANJI;
    if (cp >= 0xFF10 && cp <= 0xFF19) return CT_DIGIT;
    if ((cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A)) return CT_LATIN;
    if (cp >= 0x3000 && cp <= 0x303F) return CT_PUNCT;      /* 。、「」 etc. */
    if (cp >= 0xFF00 && cp <= 0xFF65) return CT_PUNCT;      /* ！？ etc. */
    return CT_OTHER;
}

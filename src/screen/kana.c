#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "kana.h"
#include "../agent/utf8.h"

static const char *const table[][2] = {
    {"a","あ"},{"i","い"},{"u","う"},{"e","え"},{"o","お"},
    {"ka","か"},{"ki","き"},{"ku","く"},{"ke","け"},{"ko","こ"},
    {"sa","さ"},{"si","し"},{"shi","し"},{"su","す"},{"se","せ"},{"so","そ"},
    {"ta","た"},{"ti","ち"},{"chi","ち"},{"tu","つ"},{"tsu","つ"},{"te","て"},{"to","と"},
    {"na","な"},{"ni","に"},{"nu","ぬ"},{"ne","ね"},{"no","の"},
    {"ha","は"},{"hi","ひ"},{"hu","ふ"},{"fu","ふ"},{"he","へ"},{"ho","ほ"},
    {"ma","ま"},{"mi","み"},{"mu","む"},{"me","め"},{"mo","も"},
    {"ya","や"},{"yu","ゆ"},{"yo","よ"},
    {"ra","ら"},{"ri","り"},{"ru","る"},{"re","れ"},{"ro","ろ"},
    {"wa","わ"},{"wi","ゐ"},{"we","ゑ"},{"wo","を"},{"nn","ん"},{"n'","ん"},
    {"ga","が"},{"gi","ぎ"},{"gu","ぐ"},{"ge","げ"},{"go","ご"},
    {"za","ざ"},{"zi","じ"},{"ji","じ"},{"zu","ず"},{"ze","ぜ"},{"zo","ぞ"},
    {"da","だ"},{"di","ぢ"},{"du","づ"},{"de","で"},{"do","ど"},
    {"ba","ば"},{"bi","び"},{"bu","ぶ"},{"be","べ"},{"bo","ぼ"},
    {"pa","ぱ"},{"pi","ぴ"},{"pu","ぷ"},{"pe","ぺ"},{"po","ぽ"},
    {"kya","きゃ"},{"kyu","きゅ"},{"kyo","きょ"},{"kye","きぇ"},
    {"sha","しゃ"},{"shu","しゅ"},{"sho","しょ"},{"she","しぇ"},{"sya","しゃ"},{"syu","しゅ"},{"syo","しょ"},
    {"cha","ちゃ"},{"chu","ちゅ"},{"cho","ちょ"},{"che","ちぇ"},{"tya","ちゃ"},{"tyu","ちゅ"},{"tyo","ちょ"},
    {"nya","にゃ"},{"nyu","にゅ"},{"nyo","にょ"},
    {"hya","ひゃ"},{"hyu","ひゅ"},{"hyo","ひょ"},
    {"mya","みゃ"},{"myu","みゅ"},{"myo","みょ"},
    {"rya","りゃ"},{"ryu","りゅ"},{"ryo","りょ"},
    {"gya","ぎゃ"},{"gyu","ぎゅ"},{"gyo","ぎょ"},
    {"ja","じゃ"},{"ju","じゅ"},{"jo","じょ"},{"je","じぇ"},{"jya","じゃ"},{"jyu","じゅ"},{"jyo","じょ"},
    {"zya","じゃ"},{"zyu","じゅ"},{"zyo","じょ"},
    {"bya","びゃ"},{"byu","びゅ"},{"byo","びょ"},
    {"pya","ぴゃ"},{"pyu","ぴゅ"},{"pyo","ぴょ"},
    {"fa","ふぁ"},{"fi","ふぃ"},{"fe","ふぇ"},{"fo","ふぉ"},
    {"va","ゔぁ"},{"vi","ゔぃ"},{"vu","ゔ"},{"ve","ゔぇ"},{"vo","ゔぉ"},
    {"tsa","つぁ"},{"tsi","つぃ"},{"tse","つぇ"},{"tso","つぉ"},
    {"thi","てぃ"},{"dhi","でぃ"},{"dhu","でゅ"},{"twu","とぅ"},{"dwu","どぅ"},
    {"wha","うぁ"},{"whi","うぃ"},{"whe","うぇ"},{"who","うぉ"},
    {"xa","ぁ"},{"xi","ぃ"},{"xu","ぅ"},{"xe","ぇ"},{"xo","ぉ"},
    {"la","ぁ"},{"li","ぃ"},{"lu","ぅ"},{"le","ぇ"},{"lo","ぉ"},
    {"xya","ゃ"},{"xyu","ゅ"},{"xyo","ょ"},{"lya","ゃ"},{"lyu","ゅ"},{"lyo","ょ"},
    {"xtu","っ"},{"ltu","っ"},{"xtsu","っ"},{"ltsu","っ"},{"xwa","ゎ"},{"lwa","ゎ"},
    {"xka","ヵ"},{"xke","ヶ"},
    {NULL, NULL}
};

static int is_vowel(int c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

static const char *lookup(const char *s)
{
    for (int i = 0; table[i][0]; i++) if (strcmp(table[i][0], s) == 0) return table[i][1];
    return NULL;
}

static int is_prefix(const char *s)
{
    size_t n = strlen(s);
    for (int i = 0; table[i][0]; i++) if (strncmp(table[i][0], s, n) == 0) return 1;
    return 0;
}

void kana_reset(struct kana *k) { memset(k, 0, sizeof *k); }

/* Append `kana` (hiragana) to out, as katakana when the chunk was capitals. */
static size_t emit(const struct kana *k, const char *kana, char *out, size_t o, size_t cap)
{
    uint32_t cp;
    size_t m;
    while ((m = utf8_decode(kana, &cp)) > 0) {
        if (k->upper && cp >= 0x3041 && cp <= 0x3096) cp += 0x60;
        char u[4];
        size_t e = utf8_encode(cp, u);
        if (o + e >= cap) return o;
        memcpy(out + o, u, e);
        o += e;
        kana += m;
    }
    return o;
}

static size_t emit_raw(char *out, size_t o, size_t cap, const char *s, size_t n)
{
    if (o + n >= cap) return o;
    memcpy(out + o, s, n);
    return o + n;
}

static void shift(struct kana *k, int n)
{
    if (n > k->n) n = k->n;
    memmove(k->pend, k->pend + n, (size_t)(k->n - n));
    k->n -= n;
    k->pend[k->n] = '\0';
    if (k->n == 0) k->upper = 0;
}

static size_t resolve(struct kana *k, char *out, size_t o, size_t cap, int final)
{
    while (k->n > 0) {
        const char *kana = lookup(k->pend);
        if (kana) { o = emit(k, kana, out, o, cap); shift(k, k->n); break; }
        if (k->n >= 2 && k->pend[0] == 'n' && !is_vowel(k->pend[1]) && k->pend[1] != 'y' && k->pend[1] != 'n' && k->pend[1] != '\'') {
            o = emit(k, "ん", out, o, cap);                      /* n + consonant */
            shift(k, 1);
            continue;
        }
        if (k->n >= 2 && k->pend[0] == k->pend[1] && !is_vowel(k->pend[0]) && k->pend[0] != 'n') {
            o = emit(k, "っ", out, o, cap);                      /* doubled consonant */
            shift(k, 1);
            continue;
        }
        if (!final && is_prefix(k->pend)) break;                 /* wait for more letters */
        if (final && k->n == 1 && k->pend[0] == 'n') { o = emit(k, "ん", out, o, cap); shift(k, 1); break; }
        o = emit_raw(out, o, cap, k->pend, 1);                   /* not romaji: pass through */
        shift(k, 1);
    }
    return o;
}

size_t kana_feed(struct kana *k, int ch, char *out, size_t cap)
{
    size_t o = 0;
    int lower = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    if (lower >= 'a' && lower <= 'z') {
        if (k->n == 0) k->upper = ch != lower;
        if (k->n >= KANA_PEND - 1) o = resolve(k, out, o, cap, 1);
        k->pend[k->n++] = (char)lower;
        k->pend[k->n] = '\0';
        o = resolve(k, out, o, cap, 0);
        out[o] = '\0';
        return o;
    }
    if (ch == '\'' && k->n == 1 && k->pend[0] == 'n') { o = emit(k, "ん", out, o, cap); shift(k, 1); out[o] = '\0'; return o; }
    o = resolve(k, out, o, cap, 1);
    const char *p = NULL;
    switch (ch) {
    case ',': p = "、"; break;
    case '.': p = "。"; break;
    case '-': p = "ー"; break;
    case '?': p = "？"; break;
    case '!': p = "！"; break;
    case '[': p = "「"; break;
    case ']': p = "」"; break;
    case '~': p = "〜"; break;
    default: break;
    }
    if (p) o = emit_raw(out, o, cap, p, strlen(p));
    else { char c = (char)ch; o = emit_raw(out, o, cap, &c, 1); }
    out[o] = '\0';
    return o;
}

size_t kana_flush(struct kana *k, char *out, size_t cap)
{
    size_t o = resolve(k, out, 0, cap, 1);
    out[o] = '\0';
    return o;
}

int kana_backspace(struct kana *k)
{
    if (k->n == 0) return 0;
    k->n--;
    k->pend[k->n] = '\0';
    if (k->n == 0) k->upper = 0;
    return 1;
}

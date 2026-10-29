#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ime.h"
#include "../agent/utf8.h"

void ime_init(struct ime *m, const struct dict *d)
{
    memset(m, 0, sizeof *m);
    m->dict = d;
    kana_reset(&m->kana);
}

void ime_free(struct ime *m)
{
    free(m->learned);
    m->learned = NULL;
    m->nlearned = 0;
}

int ime_active(const struct ime *m) { return m->pre[0] || m->kana.n > 0 || m->converting; }

/* ---- the user dictionary */

static struct ime_learned *learned_find(const struct ime *m, const char *reading, const char *word)
{
    for (int i = m->nlearned - 1; i >= 0; i--)
        if (strcmp(m->learned[i].reading, reading) == 0 && (!word || strcmp(m->learned[i].word, word) == 0)) return &m->learned[i];
    return NULL;
}

static void learned_add(struct ime *m, const char *reading, const char *word, int count)
{
    if (!reading[0] || !word[0] || strlen(reading) >= 64 || strlen(word) >= DICT_CAND || strcmp(reading, word) == 0) return;
    struct ime_learned *e = learned_find(m, reading, word);
    if (e) {
        e->count += count;
        /* the latest choice moves to the end: newest wins among equals */
        struct ime_learned t = *e;
        memmove(e, e + 1, (size_t)(m->learned + m->nlearned - e - 1) * sizeof *e);
        m->learned[m->nlearned - 1] = t;
        return;
    }
    if (!m->learned) { m->learned = calloc(IME_LEARN, sizeof *m->learned); if (!m->learned) return; }
    if (m->nlearned == IME_LEARN) { memmove(m->learned, m->learned + 1, (IME_LEARN - 1) * sizeof *m->learned); m->nlearned--; }
    e = &m->learned[m->nlearned++];
    snprintf(e->reading, sizeof e->reading, "%s", reading);
    snprintf(e->word, sizeof e->word, "%s", word);
    e->count = count;
}

void ime_set_learning(struct ime *m, const char *path)
{
    snprintf(m->learn_path, sizeof m->learn_path, "%s", path ? path : "");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        char *tab2 = strchr(tab, '\t');
        int count = 1;
        if (tab2) { *tab2++ = '\0'; count = atoi(tab2); }
        learned_add(m, line, tab, count > 0 ? count : 1);
    }
    fclose(f);
}

static void learned_save(const struct ime *m)
{
    if (!m->learn_path[0]) return;
    char tmp[560];
    snprintf(tmp, sizeof tmp, "%s.tmp", m->learn_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < m->nlearned; i++) fprintf(f, "%s\t%s\t%d\n", m->learned[i].reading, m->learned[i].word, m->learned[i].count);
    fclose(f);
    rename(tmp, m->learn_path);
}

/* ---- cutting the preedit into 文節 */

/* Particles and endings that may follow a word inside its segment. */
static const char *const attach[] = {
    "は", "が", "を", "に", "へ", "と", "の", "で", "も", "や", "か", "ね", "よ", "な", "さ", "ぞ", "ぜ", "わ", "なあ", "ねえ", "かなあ",
    "から", "まで", "より", "だけ", "など", "ほど", "しか", "でも", "って", "とか", "たち", "さん", "くん", "ちゃん", "さま",
    "だ", "です", "でした", "でしょう", "だった", "じゃ", "じゃない", "ない",
    "なら", "のに", "ので", "けど", "けれど", "しい", "だね", "だよ", "かな", "よね", "だろう", "らしい", "みたい", "ください",
    NULL
};
/* Common verbs whose stem is one kana (見る, 来る, 食べる): their okurigana
 * split costs nothing extra; other one-kana stems are rarely meant. */
static const char *const verb1[] = {               /* the stem kana, then the okurigana letters it takes */
    "みrt", "くrt", "でrt", "いrtkuw", "ねrtm", "きrtk", "かkitue", "よmnb", "たbtc", "のmnrb", "まtc", "あurtgk", "もtc", "しrnt", "なrtki",
    "すrmtw", "とrtbm", "つkit", "おkismcr", "だs", "えrt", "うtmk", "ふrtm", "やrtmk", "にrt", "ぬrg", "はrtnk", "ほrs", "けrs", "さkrs", NULL
};
static int in_verb1(const char *key)                 /* key = stem kana (3 bytes) + letter */
{
    for (int i = 0; verb1[i]; i++) if (strncmp(verb1[i], key, 3) == 0 && strchr(verb1[i] + 3, key[3])) return 1;
    return 0;
}

/* Words the OS and everyday talk need whose SKK order is unlucky, or that
 * are katakana: offered first, as if the user had chosen them once. */
static const struct { const char *reading, *word; } prefer[] = {
    {"すき", "好き"}, {"きのう", "昨日"}, {"なんじ", "何時"}, {"みて", "見て"}, {"かいて", "書いて"}, {"ふる", "降る"}, {"いま", "今"},
    {"きょう", "今日"}, {"あした", "明日"}, {"あさって", "明後日"}, {"おととい", "一昨日"}, {"げんき", "元気"}, {"なまえ", "名前"},
    {"こーひー", "コーヒー"}, {"ぺいんと", "ペイント"}, {"ぶらうざ", "ブラウザ"}, {"しぇる", "シェル"}, {"めにゅー", "メニュー"}, {"はーと", "ハート"},
    {"ふぁいる", "ファイル"}, {"ぷろせす", "プロセス"}, {"めもり", "メモリ"}, {"でぃすく", "ディスク"}, {"こまんど", "コマンド"}, {"りせっと", "リセット"},
    {"すてーたす", "ステータス"}, {"らーめん", "ラーメン"}, {"かれー", "カレー"}, {"ぱそこん", "パソコン"}, {"ねこ", "猫"}, {"いぬ", "犬"},
    {"わたし", "私"}, {"きみ", "君"}, {"えいが", "映画"}, {"ともだち", "友達"}, {"がっこう", "学校"}, {"しごと", "仕事"}, {"かいしゃ", "会社"},
    {NULL, NULL}
};

/* Endings that make a verb of a noun (勉強する): allowed, but a real verb
 * with its okurigana (話した) is preferred to them (花した). */
static const char *const attach_verb[] = {
    "する", "した", "して", "します", "しました", "しません", "しない", "したい", "できる", "できた", "される", "させる", "しよう", NULL
};

static int is_hira(uint32_t cp) { return (cp >= 0x3041 && cp <= 0x3096) || cp == 0x30FC; }   /* incl. ー */

/* Can `s` (n bytes) be made of up to three attachments? 1 yes, 2 yes with
 * a verb ending, 0 no. */
static int attachments_depth(const char *s, size_t n, int depth)
{
    if (n == 0) return 1;
    if (depth == 0) return 0;
    for (int i = 0; attach[i]; i++) {
        size_t k = strlen(attach[i]);
        if (k <= n && memcmp(s, attach[i], k) == 0) { int r = attachments_depth(s + k, n - k, depth - 1); if (r) return r; }
    }
    for (int i = 0; attach_verb[i]; i++) {
        size_t k = strlen(attach_verb[i]);
        if (k <= n && memcmp(s, attach_verb[i], k) == 0 && attachments_depth(s + k, n - k, depth - 1)) return 2;
    }
    return 0;
}
static int attachments(const char *s, size_t n) { return attachments_depth(s, n, 3); }

/* What may follow the okurigana kana of a stem (食べ+ました, 書い+て, 読+む). */
static const char *const inflect[] = {
    "", "る", "た", "て", "で", "だ", "ます", "ました", "ません", "ましょう", "ない", "なかった", "なくて", "たい", "たく", "たかった",
    "れる", "られる", "せる", "させる", "う", "よう", "ば", "れば", "ている", "でいる", "てる", "でる", "ていた", "でいた", "たら", "だら",
    "たり", "だり", "った", "って", "んだ", "んで", "いた", "いて", "かった", "くて", "ければ", "ちゃう", "じゃう", "き", "く", "け", "し", "す",
    "せ", "ち", "つ", "め", "み", "む", "り", "れ", "ろ", "ん", "える", "けて", "めて", "れて", "せて", "して", "いた", "いだ", "いで", "った",
    NULL
};

static int attachments_depth(const char *s, size_t n, int depth);
static int is_small(uint32_t cp) { return cp == 0x3041 || cp == 0x3043 || cp == 0x3045 || cp == 0x3047 || cp == 0x3049 || cp == 0x3083 || cp == 0x3085 || cp == 0x3087 || cp == 0x308E; }

/* Can `s` be okurigana: one kana (not small), one of the endings above and
 * then particles? */
static int inflections(const char *s, size_t n)
{
    if (n == 0) return 0;
    uint32_t cp;
    size_t k0 = utf8_decode(s, &cp);
    if (!k0 || is_small(cp) || k0 > n) return 0;
    s += k0;
    n -= k0;
    for (int i = 0; inflect[i]; i++) {
        size_t k = strlen(inflect[i]);
        if (k > n || memcmp(s, inflect[i], k) != 0) continue;
        if (attachments_depth(s + k, n - k, 2) == 1) return 1;
    }
    return 0;
}

struct cut { size_t pos[IME_PRE]; int n; };            /* byte offset of each code point, and one past the end */

static void cut_points(const char *s, struct cut *c)
{
    c->n = 0;
    size_t i = 0, len = strlen(s);
    while (i < len && c->n < IME_PRE - 1) {
        c->pos[c->n++] = i;
        uint32_t cp;
        size_t k = utf8_decode(s + i, &cp);
        if (!k) break;
        i += k;
    }
    c->pos[c->n] = len;
}

static int all_hira(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        size_t k = utf8_decode(s + i, &cp);
        if (!k || !is_hira(cp)) return 0;
        i += k;
    }
    return 1;
}

static void to_katakana(const char *hira, size_t n, char *out, size_t cap)
{
    size_t o = 0, i = 0;
    while (i < n) {
        uint32_t cp;
        size_t k = utf8_decode(hira + i, &cp);
        if (!k) break;
        if (cp >= 0x3041 && cp <= 0x3096) cp += 0x60;
        char u[4];
        size_t e = utf8_encode(cp, u);
        if (o + e >= cap) break;
        memcpy(out + o, u, e);
        o += e;
        i += k;
    }
    out[o] = '\0';
}

static int add_cand(struct ime_seg *g, const char *word, const char *suffix)
{
    if (g->ncands >= IME_CANDS || strlen(word) + strlen(suffix) >= DICT_CAND) return 0;
    char c[DICT_CAND * 2];
    snprintf(c, sizeof c, "%s%s", word, suffix);
    for (int i = 0; i < g->ncands; i++) if (strcmp(g->cands[i], c) == 0) return 0;
    g->sufl[g->ncands] = strlen(suffix);
    snprintf(g->cands[g->ncands++], DICT_CAND, "%.*s", DICT_CAND - 1, c);
    return 1;
}

/* Put `word` in front of the candidates. */
static void first_cand(struct ime_seg *g, const char *word)
{
    for (int i = 0; i < g->ncands; i++)
        if (strcmp(g->cands[i], word) == 0) { memmove(g->cands + 1, g->cands, (size_t)i * DICT_CAND); memmove(g->sufl + 1, g->sufl, (size_t)i * sizeof g->sufl[0]); snprintf(g->cands[0], DICT_CAND, "%s", word); g->sufl[0] = 0; return; }
    if (g->ncands == IME_CANDS) g->ncands--;
    memmove(g->cands + 1, g->cands, (size_t)g->ncands * DICT_CAND);
    memmove(g->sufl + 1, g->sufl, (size_t)g->ncands * sizeof g->sufl[0]);
    snprintf(g->cands[0], DICT_CAND, "%.*s", DICT_CAND - 1, word);
    g->sufl[0] = 0;
    g->ncands++;
}

static void add_list(struct ime_seg *g, const char *list, const char *suffix)
{
    while (*list && g->ncands < IME_CANDS) {
        const char *sl = strchr(list, '/');
        size_t len = sl ? (size_t)(sl - list) : strlen(list);
        if (len > 0 && len < DICT_CAND) {
            char w[DICT_CAND];
            memcpy(w, list, len);
            w[len] = '\0';
            add_cand(g, w, suffix);
        }
        if (!sl) break;
        list = sl + 1;
    }
}

/* Hiragana words that stay hiragana (with particles and endings after them). */
static const char *const plain[] = {
    "あります", "います", "ありました", "いました", "ありません", "いません", "ある", "いる", "する", "した", "して", "します", "しました",
    "ください", "ありがとう", "ございます", "こんにちは", "こんばんは", "おはよう", "おやすみ", "さようなら", "ただいま", "おかえり",
    "いい", "よい", "この", "その", "あの", "どの", "これ", "それ", "あれ", "どれ", "ここ", "そこ", "あそこ", "どこ", "こう", "そう", "どう",
    "こと", "もの", "ため", "よう", "とき", "ところ", "ずっと", "とても", "すごく", "もっと", "ちょっと", "また", "まだ", "もう", "よく",
    "たぶん", "きっと", "ぜひ", "いつも", "なんか", "なんて", "すぐ", "だから", "でも", "けど", "そして", "それで", "やっぱり", "ほんとう",
    "うん", "はい", "いいえ", "ええ", "ねえ", "ほら", "やあ", "わたし", "ぼく", "あなた", "きみ", "だれ", "なに", "なん", "いつ", "なぜ", "どうして",
    NULL
};

static int in_plain(const char *s, size_t n)
{
    for (int i = 0; plain[i]; i++) if (strlen(plain[i]) == n && memcmp(plain[i], s, n) == 0) return 1;
    return 0;
}

static int list_count(const char *list)
{
    int n = 1;
    for (; *list; list++) if (*list == '/') n++;
    return n;
}

struct option { double cost; const char *list; char word[DICT_CAND]; char suffix[DICT_CAND]; };

/* `list` is a dictionary line ("c1/c2/…") or, when NULL, `word` alone. */
static void option_add(struct option *opts, int *n, double cost, const char *list, const char *word, const char *suffix, size_t suffix_n)
{
    if (*n >= 32 || suffix_n >= DICT_CAND || (word && strlen(word) >= DICT_CAND)) return;
    struct option *o = &opts[(*n)++];
    o->cost = cost;
    o->list = list;
    snprintf(o->word, sizeof o->word, "%s", word ? word : "");
    memcpy(o->suffix, suffix, suffix_n);
    o->suffix[suffix_n] = '\0';
}

/* The candidates of one segment reading `r` (n bytes, code points i0..i1 of
 * the cut), best first, and the cost of the segment for the cut search:
 * cheap for a learned word, a dictionary word with its particles or its
 * okurigana, or a hiragana word everyone writes in hiragana; dear for kana
 * nobody knows. Longer segments and readings with many candidates (common
 * words) are cheaper. `g` may be NULL when only the cost is wanted. */
static double segment(const struct ime *m, const char *r, size_t n, const struct cut *c, int i0, int i1, struct ime_seg *g)
{
    int kana = i1 - i0;
    char key[IME_PRE + 2];
    const char *list;
    struct option opts[32];
    int nopt = 0;
    double unknown;                                   /* nobody knows it: hiragana as typed */
    int loan = memmem(r, n, "ー", 3) != NULL;        /* a long vowel mark: a loanword, katakana */
    if (g) g->ncands = 0;
    if (n >= sizeof key - 2) return 100;
    memcpy(key, r, n);
    key[n] = '\0';

    /* learned: the whole reading, newest first */
    double cost = 100;
    int nlearn = 0;
    const char *learned_words[4];
    for (int i = m->nlearned - 1; i >= 0 && nlearn < 4; i--) {
        const struct ime_learned *e = &m->learned[i];
        if (strcmp(e->reading, key) != 0) continue;
        learned_words[nlearn++] = e->word;
    }
    for (int i = 0; prefer[i].reading; i++)
        if (strcmp(prefer[i].reading, key) == 0) option_add(opts, &nopt, 0.55 - 0.15 * (kana < 4 ? kana : 4), NULL, prefer[i].word, "", 0);
    if (nlearn) cost = 0.5 - 0.1 * kana;
    if (!all_hira(r, n)) {                            /* punctuation, katakana, letters: a segment of its own, kept as is */
        if (g) add_cand(g, key, "");
        return kana == 1 ? 1.0 : 1.0 + 0.5 * kana;
    }
    const struct dict none = {0};
    const struct dict *d = m->dict ? m->dict : &none;
    unknown = loan ? 0.6 + 0.15 * kana : 1.2 + 0.2 * kana;
    if (loan) { char kata[DICT_CAND]; to_katakana(r, n, kata, sizeof kata); option_add(opts, &nopt, unknown, NULL, kata, "", 0); }   /* a loanword: katakana */
#define POP(list) (0.08 * (list_count(list) < 5 ? list_count(list) : 5))     /* many candidates: a common reading */
    double bonus = -0.15 * (kana < 4 ? kana : 4);     /* longer is better, up to a point (rare long entries) */
    /* the whole reading as one dictionary word */
    if ((list = dict_lookup(d, key)) != NULL)
        option_add(opts, &nopt, 1.0 + bonus + (kana == 1 ? 1.5 : 0) - POP(list), list, NULL, "", 0);
    if (in_plain(r, n)) cost = cost < 0.5 + bonus ? cost : 0.5 + bonus;
    /* a word followed by particles / endings; a stem with its okurigana */
    for (int k = i1 - 1; k > i0; k--) {
        size_t stem = c->pos[k] - c->pos[i0];
        const char *rest = r + stem;
        size_t rest_n = n - stem;
        int stem_kana = k - i0;
        memcpy(key, r, stem);
        key[stem] = '\0';
        int at = attachments(rest, rest_n);
        double abonus = -0.15 * (stem_kana < 4 ? stem_kana : 4) - 0.03 * (kana - stem_kana) + (at == 2 ? 0.3 : 0);
        if (at) {
            for (int i = 0; prefer[i].reading; i++)
                if (strcmp(prefer[i].reading, key) == 0) option_add(opts, &nopt, 0.65 + abonus, NULL, prefer[i].word, rest, rest_n);
            for (int i = m->nlearned - 1; i >= 0; i--)                                  /* a learned word with new particles */
                if (strcmp(m->learned[i].reading, key) == 0) { option_add(opts, &nopt, 0.5 + abonus, NULL, m->learned[i].word, rest, rest_n); break; }
            if ((list = dict_lookup(d, key)) != NULL)
                option_add(opts, &nopt, 1.1 + abonus + (stem_kana == 1 ? 1.5 : 0) - POP(list), list, NULL, rest, rest_n);
            if (in_plain(r, stem)) cost = cost < 0.55 + abonus ? cost : 0.55 + abonus;
            int lstem = memmem(r, stem, "ー", 3) != NULL;
            double u = (lstem ? 0.6 + 0.15 * stem_kana : 1.2 + 0.2 * stem_kana) - 0.03 * (kana - stem_kana) + (at == 2 ? 0.3 : 0);   /* an unknown word with its particles */
            if (u < unknown) unknown = u;
            if (lstem) { char kata[DICT_CAND]; to_katakana(r, stem, kata, sizeof kata); option_add(opts, &nopt, u, NULL, kata, rest, rest_n); }
        }
        uint32_t cp;
        utf8_decode(rest, &cp);
        char letter = dict_okuri_letter(cp);
        if (letter && i1 - k <= 7 && inflections(rest, rest_n)) {
            key[stem] = letter;
            key[stem + 1] = '\0';
            if ((list = dict_lookup(d, key)) != NULL)
                option_add(opts, &nopt, 1.0 - 0.15 * (stem_kana < 4 ? stem_kana : 4) + (stem_kana == 1 && !in_verb1(key) ? 0.4 : 0) - POP(list), list, NULL, rest, rest_n);
        }
    }
    for (int i = 0; i < nopt; i++) if (opts[i].cost < cost) cost = opts[i].cost;
    if (unknown < cost) cost = unknown;
    if (g) {
        for (int done = 0; done < nopt; done++) {     /* cheapest option's candidates first */
            int b = -1;
            for (int i = 0; i < nopt; i++) if (opts[i].cost < 50 && (b < 0 || opts[i].cost < opts[b].cost)) b = i;
            if (b < 0) break;
            if (opts[b].list) add_list(g, opts[b].list, opts[b].suffix); else add_cand(g, opts[b].word, opts[b].suffix);
            opts[b].cost = 100;
        }
        memcpy(key, r, n);
        key[n] = '\0';
        char kata[DICT_CAND];
        to_katakana(r, n, kata, sizeof kata);
        add_cand(g, key, "");
        add_cand(g, kata, "");
        if (in_plain(r, n) || kana == 1) first_cand(g, key);   /* a hiragana word, or one kana, is meant as is */
        for (int i = 0; i < 4 && i < nlearn; i++) first_cand(g, learned_words[nlearn - 1 - i]);   /* what the user chose, newest first */
    }
    return cost;
}

#define MAX_SEG_KANA 20

/* Cut `pre` from code point `from` to the end into the cheapest segments,
 * appending them to m->segs. */
static void cut_from(struct ime *m, const struct cut *c, int from)
{
    int n = c->n;
    double best[IME_PRE + 1];
    int back[IME_PRE + 1];
    for (int i = 0; i <= n; i++) { best[i] = 1e9; back[i] = -1; }
    best[from] = 0;
    for (int i = from; i < n; i++) {
        if (best[i] >= 1e9) continue;
        for (int j = i + 1; j <= n && j - i <= MAX_SEG_KANA; j++) {
            double cst = segment(m, m->pre + c->pos[i], c->pos[j] - c->pos[i], c, i, j, NULL);
            if (best[i] + cst < best[j]) { best[j] = best[i] + cst; back[j] = i; }
        }
    }
    int bounds[IME_PRE + 1], nb = 0;
    for (int j = n; j > from && back[j] >= 0; j = back[j]) bounds[nb++] = j;
    int start = from;
    for (int k = nb - 1; k >= 0 && m->nsegs < IME_SEGS; k--) {
        int end = bounds[k];
        struct ime_seg *g = &m->segs[m->nsegs++];
        g->off = c->pos[start];
        g->len = c->pos[end] - c->pos[start];
        g->cur = 0;
        segment(m, m->pre + g->off, g->len, c, start, end, g);
        start = end;
    }
}

static void recut(struct ime *m, int keep)
{
    struct cut c;
    cut_points(m->pre, &c);
    m->nsegs = keep;
    int from = 0;
    if (keep > 0) {
        size_t off = m->segs[keep - 1].off + m->segs[keep - 1].len;
        for (from = 0; from < c.n && c.pos[from] < off; from++) ;
    }
    cut_from(m, &c, from);
}

static void convert(struct ime *m)
{
    m->nsegs = 0;
    recut(m, 0);
    m->seg = 0;
    m->converting = m->nsegs > 0;
}

/* Shift+←/→: the current segment gives a kana to the next, or takes one. */
static void resize(struct ime *m, int delta)
{
    struct ime_seg *g = &m->segs[m->seg];
    struct cut c;
    cut_points(m->pre, &c);
    int i0 = 0, i1 = 0;
    while (i0 < c.n && c.pos[i0] < g->off) i0++;
    while (i1 < c.n && c.pos[i1] < g->off + g->len) i1++;
    int n1 = i1 + delta;
    if (n1 <= i0 || n1 > c.n) return;
    g->len = c.pos[n1] - g->off;
    g->cur = 0;
    segment(m, m->pre + g->off, g->len, &c, i0, n1, g);
    int keep = m->seg + 1;
    m->nsegs = keep;
    if (n1 < c.n) cut_from(m, &c, n1);
}

/* ---- keys */

static void pre_append(struct ime *m, const char *s)
{
    size_t n = strlen(m->pre), k = strlen(s);
    if (n + k >= sizeof m->pre) return;
    memcpy(m->pre + n, s, k + 1);
}

/* Move the romaji still pending into the preedit. */
static void settle(struct ime *m)
{
    char out[32];
    size_t n = kana_flush(&m->kana, out, sizeof out);
    out[n] = '\0';
    if (n) pre_append(m, out);
}

static size_t put(char *out, size_t o, size_t cap, const char *s)
{
    size_t k = strlen(s);
    if (o + k >= cap) return o;
    memcpy(out + o, s, k);
    out[o + k] = '\0';
    return o + k;
}

/* Commit all segments as shown, and learn what was chosen. */
static size_t commit_segments(struct ime *m, char *out, size_t o, size_t cap)
{
    char whole[IME_PRE * 3] = "";
    int changed = 0;
    for (int i = 0; i < m->nsegs; i++) {
        struct ime_seg *g = &m->segs[i];
        const char *w = g->cands[g->cur];
        o = put(out, o, cap, w);
        size_t wl = strlen(whole);
        snprintf(whole + wl, sizeof whole - wl, "%s", w);
        char reading[IME_PRE];
        snprintf(reading, sizeof reading, "%.*s", (int)g->len, m->pre + g->off);
        if (strcmp(w, reading) == 0 || !all_hira(reading, g->len)) continue;   /* kana as typed: nothing to learn */
        learned_add(m, reading, w, 1);
        changed = 1;
    }
    if (m->nsegs > 1 && all_hira(m->pre, strlen(m->pre)) && strcmp(whole, m->pre) != 0) { learned_add(m, m->pre, whole, 1); changed = 1; }
    if (changed) learned_save(m);
    m->pre[0] = '\0';
    m->converting = 0;
    m->nsegs = 0;
    return o;
}

static size_t commit_all(struct ime *m, char *out, size_t o, size_t cap)
{
    if (m->converting) o = commit_segments(m, out, o, cap);
    settle(m);
    o = put(out, o, cap, m->pre);
    m->pre[0] = '\0';
    return o;
}

static void pre_backspace(struct ime *m)
{
    size_t n = strlen(m->pre);
    while (n > 0 && ((unsigned char)m->pre[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    m->pre[n] = '\0';
}

size_t ime_key(struct ime *m, const char *text, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    unsigned char c = (unsigned char)text[0];
    int single = text[1] == '\0';

    if (m->converting) {
        struct ime_seg *g = &m->segs[m->seg];
        if (strcmp(text, "\x1b[C") == 0 || (single && c == '\t')) { if (m->seg + 1 < m->nsegs) m->seg++; return 0; }
        if (strcmp(text, "\x1b[D") == 0) { if (m->seg > 0) m->seg--; return 0; }
        if (strcmp(text, "\x1b[1;2C") == 0) { resize(m, 1); return 0; }
        if (strcmp(text, "\x1b[1;2D") == 0) { resize(m, -1); return 0; }
        if (strcmp(text, "\x1b[A") == 0) { g->cur = (g->cur + g->ncands - 1) % g->ncands; return 0; }
        if (strcmp(text, "\x1b[B") == 0 || (single && c == ' ')) { g->cur = (g->cur + 1) % g->ncands; return 0; }
        if (single && c == 0x7F) { m->converting = 0; m->nsegs = 0; return 0; }         /* back to the kana */
        if (single && c == 0x1B) { m->converting = 0; m->nsegs = 0; return 0; }
        if (single && c == '\r') return commit_segments(m, out, 0, cap);              /* what is shown, nothing else sent */
        if (single && c >= '1' && c <= '9') {                                         /* pick by number, on to the next segment */
            int i = (g->cur / 9) * 9 + (c - '1');
            if (i >= g->ncands) return 0;
            g->cur = i;
            if (m->seg + 1 < m->nsegs) { m->seg++; return 0; }
            return commit_segments(m, out, 0, cap);
        }
        o = commit_segments(m, out, 0, cap);                                           /* anything else: commit, then handle */
    }
    if (single && c == 0x7F) {                                   /* Backspace */
        if (kana_backspace(&m->kana)) return o;
        if (m->pre[0]) { pre_backspace(m); return o; }
        return put(out, o, cap, text);
    }
    if (single && c == '\r') {                                    /* Enter */
        if (ime_active(m)) { settle(m); o = put(out, o, cap, m->pre); m->pre[0] = '\0'; }
        return put(out, o, cap, text);                                /* plain kana: commit and send the line */
    }
    if (single && c == 0x1B) {                                    /* Escape */
        if (ime_active(m)) { kana_reset(&m->kana); m->pre[0] = '\0'; return o; }
        return put(out, o, cap, text);
    }
    if (single && c == ' ') {                                     /* Space: convert */
        settle(m);
        if (!m->pre[0]) return put(out, o, cap, text);
        convert(m);
        return o;
    }
    if (single && c >= 0x20 && c < 0x7F) {                         /* a letter: more preedit */
        char kana[64];
        size_t n = kana_feed(&m->kana, c, kana, sizeof kana);
        kana[n] = '\0';
        if (n) pre_append(m, kana);
        return o;
    }
    /* anything else (arrows, control): commit and pass it on */
    o = commit_all(m, out, o, cap);
    return put(out, o, cap, text);
}

size_t ime_flush(struct ime *m, char *out, size_t cap)
{
    out[0] = '\0';
    return commit_all(m, out, 0, cap);
}

void ime_shown(const struct ime *m, char *buf, size_t cap, size_t *sel_off, size_t *sel_len)
{
    buf[0] = '\0';
    *sel_off = *sel_len = 0;
    if (m->converting) {
        size_t o = 0;
        for (int i = 0; i < m->nsegs; i++) {
            const char *w = m->segs[i].cands[m->segs[i].cur];
            if (i == m->seg) { *sel_off = o; *sel_len = strlen(w); }
            o = put(buf, o, cap, w);
        }
    } else {
        snprintf(buf, cap, "%s%s", m->pre, m->kana.pend);
    }
}

void ime_popup(const struct ime *m, char *buf, size_t cap, int *cur_index)
{
    buf[0] = '\0';
    *cur_index = 0;
    if (!m->converting) return;
    const struct ime_seg *g = &m->segs[m->seg];
    *cur_index = g->cur;
    size_t o = 0;
    int first = (g->cur / 9) * 9;
    for (int i = first; i < g->ncands && i < first + 9; i++) {
        char item[DICT_CAND + 8];
        snprintf(item, sizeof item, "%s%d %s", i > first ? "  " : "", i - first + 1, g->cands[i]);
        o = put(buf, o, cap, item);
    }
    if (g->ncands > first + 9 || first > 0) o = put(buf, o, cap, "  …");
}

void ime_cut(const struct ime *m, char *buf, size_t cap)
{
    buf[0] = '\0';
    size_t o = 0;
    for (int i = 0; i < m->nsegs; i++) {
        if (i) o = put(buf, o, cap, "|");
        o = put(buf, o, cap, m->segs[i].cands[m->segs[i].cur]);
    }
}

void ime_convert_text(struct ime *m, const char *hira, char *out, size_t cap)
{
    snprintf(m->pre, sizeof m->pre, "%s", hira);
    convert(m);
    ime_cut(m, out, cap);
    m->converting = 0;
    m->nsegs = 0;
    m->pre[0] = '\0';
}


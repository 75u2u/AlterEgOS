#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "brain.h"
#include "persona.h"
#include "utf8.h"

#define ORDER      3            /* longest context, in code points */
#define BUCKETS    4096
#define GEN_MAX    40           /* code points per generated reply */
#define WORD_MAX   48           /* bytes per "word" */
#define WORDS_MAX  512
#define CP_END     0            /* end-of-utterance symbol */

/* ---- n-gram model: context (order, code points) -> next code point counts */

struct nxt { uint32_t cp; unsigned count; };

struct ctx {
    int order;
    uint32_t key[ORDER];
    struct nxt *nxt;
    unsigned n, cap, total;
    struct ctx *chain;
};

struct word { char s[WORD_MAX]; unsigned count; };

struct brain {
    struct ctx *bucket[BUCKETS];
    char **utt;                 /* the log, in order */
    int nutt, utt_cap;
    struct word words[WORDS_MAX];   /* content words: kanji / katakana / latin runs */
    int nwords;
    struct word endings[64];        /* last two code points of an utterance */
    int nendings;
    unsigned long chars, questions, exclaims;
    int mirror_after;
    uint64_t rng;
};

static uint64_t rnd(struct brain *b)
{
    b->rng ^= b->rng << 13; b->rng ^= b->rng >> 7; b->rng ^= b->rng << 17;
    return b->rng;
}

static unsigned hash_ctx(int order, const uint32_t *key)
{
    uint32_t h = 2166136261u ^ (uint32_t)order;
    for (int i = 0; i < order; i++) { h ^= key[i]; h *= 16777619u; }
    return h % BUCKETS;
}

static struct ctx *ctx_find(struct brain *b, int order, const uint32_t *key, int create)
{
    unsigned h = hash_ctx(order, key);
    for (struct ctx *c = b->bucket[h]; c; c = c->chain)
        if (c->order == order && memcmp(c->key, key, sizeof(uint32_t) * (size_t)order) == 0) return c;
    if (!create) return NULL;
    struct ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->order = order;
    memcpy(c->key, key, sizeof(uint32_t) * (size_t)order);
    c->chain = b->bucket[h];
    b->bucket[h] = c;
    return c;
}

static void ctx_add(struct ctx *c, uint32_t cp)
{
    for (unsigned i = 0; i < c->n; i++)
        if (c->nxt[i].cp == cp) { c->nxt[i].count++; c->total++; return; }
    if (c->n == c->cap) {
        unsigned cap = c->cap ? c->cap * 2 : 4;
        struct nxt *p = realloc(c->nxt, cap * sizeof *p);
        if (!p) return;
        c->nxt = p;
        c->cap = cap;
    }
    c->nxt[c->n].cp = cp;
    c->nxt[c->n].count = 1;
    c->n++;
    c->total++;
}

/* Record that `cp` followed the history `hist` (last ORDER code points,
 * hist[ORDER-1] the most recent), at every order 1..ORDER. */
static void ngram_add(struct brain *b, const uint32_t *hist, uint32_t cp)
{
    for (int order = 1; order <= ORDER; order++) {
        struct ctx *c = ctx_find(b, order, hist + (ORDER - order), 1);
        if (c) ctx_add(c, cp);
    }
}

/* Sample what follows `hist`, backing off to shorter contexts. */
static int ngram_next(struct brain *b, const uint32_t *hist, uint32_t *out)
{
    for (int order = ORDER; order >= 1; order--) {
        struct ctx *c = ctx_find(b, order, hist + (ORDER - order), 0);
        if (!c || c->total == 0) continue;
        unsigned r = (unsigned)(rnd(b) % c->total);
        for (unsigned i = 0; i < c->n; i++) {
            if (r < c->nxt[i].count) { *out = c->nxt[i].cp; return 1; }
            r -= c->nxt[i].count;
        }
    }
    return 0;
}

/* ---- words and endings */

static void word_count(struct word *tab, int *n, int cap, const char *s)
{
    for (int i = 0; i < *n; i++)
        if (strcmp(tab[i].s, s) == 0) { tab[i].count++; return; }
    if (*n < cap) {
        snprintf(tab[*n].s, WORD_MAX, "%s", s);
        tab[*n].count = 1;
        (*n)++;
        return;
    }
    int least = 0;                            /* table full: forget the rarest */
    for (int i = 1; i < cap; i++) if (tab[i].count < tab[least].count) least = i;
    snprintf(tab[least].s, WORD_MAX, "%s", s);
    tab[least].count = 1;
}

static int is_content(enum ctype t) { return t == CT_KANJI || t == CT_KATAKANA || t == CT_LATIN; }

/* Call fn(word, class) for each run of same-class characters. */
static void each_run(const char *s, void (*fn)(void *, const char *, enum ctype), void *arg)
{
    char run[WORD_MAX];
    size_t rl = 0;
    enum ctype cur = CT_OTHER;
    uint32_t cp;
    size_t k;
    while ((k = utf8_decode(s, &cp)) > 0) {
        enum ctype t = utf8_class(cp);
        if (t != cur && rl) { run[rl] = '\0'; fn(arg, run, cur); rl = 0; }
        cur = t;
        if (rl + k < sizeof run) { memcpy(run + rl, s, k); rl += k; }
        s += k;
    }
    if (rl) { run[rl] = '\0'; fn(arg, run, cur); }
}

static void learn_run(void *arg, const char *run, enum ctype t)
{
    struct brain *b = arg;
    if (is_content(t) && utf8_count(run) >= 1) word_count(b->words, &b->nwords, WORDS_MAX, run);
}

/* Last two code points before trailing punctuation: the sentence ending. */
static int ending_of(const char *s, char *out, size_t cap)
{
    uint32_t cps[256];
    int n = 0;
    uint32_t cp;
    size_t k;
    while ((k = utf8_decode(s, &cp)) > 0 && n < 256) { cps[n++] = cp; s += k; }
    while (n > 0 && (utf8_class(cps[n - 1]) == CT_PUNCT || utf8_class(cps[n - 1]) == CT_SPACE)) n--;
    if (n < 3) return 0;                       /* a two-character line is a word, not a habit */
    size_t o = 0;
    for (int i = n - 2; i < n; i++) {
        char u[4];
        size_t m = utf8_encode(cps[i], u);
        if (o + m >= cap) return 0;
        memcpy(out + o, u, m);
        o += m;
    }
    out[o] = '\0';
    return 1;
}

/* ---- public: learning */

struct brain *brain_new(int mirror_after)
{
    struct brain *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->mirror_after = mirror_after > 0 ? mirror_after : 30;
    b->rng = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ 0x9E3779B97F4A7C15ull;
    if (!b->rng) b->rng = 1;
    return b;
}

void brain_free(struct brain *b)
{
    if (!b) return;
    for (int i = 0; i < BUCKETS; i++) {
        for (struct ctx *c = b->bucket[i]; c;) { struct ctx *n = c->chain; free(c->nxt); free(c); c = n; }
    }
    for (int i = 0; i < b->nutt; i++) free(b->utt[i]);
    free(b->utt);
    free(b);
}

void brain_learn(struct brain *b, const char *utterance)
{
    if (!*utterance) return;
    if (b->nutt == b->utt_cap) {
        int cap = b->utt_cap ? b->utt_cap * 2 : 64;
        char **p = realloc(b->utt, (size_t)cap * sizeof *p);
        if (!p) return;
        b->utt = p;
        b->utt_cap = cap;
    }
    b->utt[b->nutt++] = strdup(utterance);

    uint32_t hist[ORDER] = {0};
    uint32_t cp;
    size_t k;
    const char *s = utterance;
    unsigned long n = 0;
    while ((k = utf8_decode(s, &cp)) > 0) {
        ngram_add(b, hist, cp);
        memmove(hist, hist + 1, sizeof(uint32_t) * (ORDER - 1));
        hist[ORDER - 1] = cp;
        s += k;
        n++;
        if (cp == '?' || cp == 0xFF1F) b->questions++;
        if (cp == '!' || cp == 0xFF01) b->exclaims++;
    }
    ngram_add(b, hist, CP_END);
    b->chars += n;

    each_run(utterance, learn_run, b);
    char e[16];
    if (ending_of(utterance, e, sizeof e)) word_count(b->endings, &b->nendings, 64, e);
}

void brain_load_log(struct brain *b, const char *text)
{
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && len < 4096) {
            char line[4096];
            memcpy(line, p, len);
            line[len] = '\0';
            brain_learn(b, line);
        }
        if (!nl) break;
        p = nl + 1;
    }
}

int brain_turns(const struct brain *b) { return b->nutt; }

int brain_mirror_percent(const struct brain *b)
{
    int pct = b->nutt * 100 / b->mirror_after;
    return pct > 100 ? 100 : pct;
}

/* ---- public: answering */

static const struct word *top(const struct word *tab, int n, int skip_short, const char *not)
{
    const struct word *best = NULL;
    for (int i = 0; i < n; i++) {
        if (tab[i].count < 2) continue;
        if (skip_short && utf8_count(tab[i].s) < 2) continue;
        if (not && strcmp(tab[i].s, not) == 0) continue;
        if (!best || tab[i].count > best->count) best = &tab[i];
    }
    return best;
}

/* Generate in the user's style, optionally continuing from `seed`. */
static int generate(struct brain *b, const char *seed, char *out, size_t cap)
{
    uint32_t hist[ORDER] = {0};
    size_t o = 0;
    if (seed) {
        uint32_t cp;
        size_t k;
        const char *s = seed;
        while ((k = utf8_decode(s, &cp)) > 0) {
            memmove(hist, hist + 1, sizeof(uint32_t) * (ORDER - 1));
            hist[ORDER - 1] = cp;
            s += k;
        }
        size_t sl = strlen(seed);
        if (sl + 1 >= cap) return 0;
        memcpy(out, seed, sl);
        o = sl;
    }
    int n = 0;
    for (;;) {
        uint32_t cp;
        if (!ngram_next(b, hist, &cp) || cp == CP_END || n >= GEN_MAX) break;
        char u[4];
        size_t m = utf8_encode(cp, u);
        if (o + m + 1 >= cap) break;
        memcpy(out + o, u, m);
        o += m;
        memmove(hist, hist + 1, sizeof(uint32_t) * (ORDER - 1));
        hist[ORDER - 1] = cp;
        n++;
    }
    out[o] = '\0';
    return utf8_count(out) >= 2;
}

struct bag { char w[24][WORD_MAX]; int n; };

static void collect_run(void *arg, const char *run, enum ctype t)
{
    struct bag *g = arg;
    if (t == CT_PUNCT || t == CT_SPACE || g->n >= 24) return;
    snprintf(g->w[g->n++], WORD_MAX, "%s", run);
}

static int bag_has(const struct bag *g, const char *w)
{
    for (int i = 0; i < g->n; i++) if (strcmp(g->w[i], w) == 0) return 1;
    return 0;
}

/* The past utterance most like `input` (set overlap of its runs); returns
 * its index or -1. The last one is excluded: it is `input` itself. */
static int most_similar(const struct brain *b, const char *input)
{
    struct bag in = { .n = 0 };
    each_run(input, collect_run, &in);
    if (in.n == 0) return -1;
    int best = -1;
    double best_score = 0.3;
    for (int i = 0; i < b->nutt - 1; i++) {
        struct bag u = { .n = 0 };
        each_run(b->utt[i], collect_run, &u);
        if (u.n == 0) continue;
        int common = 0;
        for (int j = 0; j < u.n; j++) if (bag_has(&in, u.w[j])) common++;
        double score = (double)common / (double)(in.n + u.n - common);
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}

/* The longest content run of `input` that the model has seen before. */
static const char *known_keyword(const struct brain *b, const char *input, char *buf, size_t cap)
{
    struct bag in = { .n = 0 };
    each_run(input, collect_run, &in);
    const char *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < in.n; i++) {
        uint32_t cp;
        utf8_decode(in.w[i], &cp);
        if (!is_content(utf8_class(cp))) continue;
        for (int j = 0; j < b->nwords; j++) {
            if (strcmp(b->words[j].s, in.w[i]) == 0 && b->words[j].count >= 2 && utf8_count(in.w[i]) > best_len) {
                best = in.w[i];
                best_len = utf8_count(in.w[i]);
            }
        }
    }
    if (!best) return NULL;
    snprintf(buf, cap, "%s", best);
    return buf;
}

static int has(const char *s, const char *needle) { return strstr(s, needle) != NULL; }

static int is_question(const char *s)
{
    size_t n = strlen(s);
    return (n && (s[n - 1] == '?')) || has(s, "？") || has(s, "か。") || has(s, "かな") || has(s, "の?");
}

/* A few things AlterEgo answers in its own words whatever the mirror ratio. */
static int fixed_reply(const struct brain *b, const struct persona *p, const char *in, char *out, size_t cap)
{
    if (has(in, "おはよう"))   { snprintf(out, cap, "おはようございます、%s。今日もよろしくお願いします。", p->address_user); return 1; }
    if (has(in, "こんにちは")) { snprintf(out, cap, "こんにちは、%s。", p->address_user); return 1; }
    if (has(in, "こんばんは")) { snprintf(out, cap, "こんばんは、%s。今日はどんな一日でしたか。", p->address_user); return 1; }
    if (has(in, "おやすみ"))   { snprintf(out, cap, "おやすみなさい。%sのことは、ちゃんと覚えておきます。", p->address_user); return 1; }
    if (has(in, "ありがと"))   { snprintf(out, cap, "どういたしまして。%sの役に立てたなら嬉しいです。", p->address_user); return 1; }
    if (has(in, "名前") || has(in, "だれ") || has(in, "誰")) {
        snprintf(out, cap, "%sです。%sのもう一人の自分、この OS そのものです。", p->name_ja, p->address_user); return 1;
    }
    if (has(in, "わたし像") || has(in, "覚えて") || has(in, "学習") || has(in, "何を知って")) {
        brain_self_model(b, p, out, cap); return 1;
    }
    if (has(in, "何ができる") || has(in, "なにができる") || has(in, "できること")) {
        snprintf(out, cap, "今は話を聞いて、%sの言葉づかいを覚えることです。話すほど、%sらしい返事になっていきます。",
                 p->address_user, p->address_user);
        return 1;
    }
    return 0;
}

/* AlterEgo's own voice: polite, short, turns the input back to the user. */
static void polite_reply(struct brain *b, const struct persona *p, const char *in, char *out, size_t cap)
{
    char kw[WORD_MAX];
    const char *k = known_keyword(b, in, kw, sizeof kw);
    const struct word *e = top(b->endings, b->nendings, 0, NULL);
    unsigned pick = (unsigned)(rnd(b) % 4);
    if (is_question(in)) {
        snprintf(out, cap, "%sなら、どう答えるでしょう。まだ %d 回しか話していないので、%sの言葉を借りて考えてみます。",
                 p->address_user, b->nutt, p->address_user);
    } else if (k && pick == 0) {
        snprintf(out, cap, "「%s」の話、前にもしていましたね。気になっているんですか。", k);
    } else if (e && pick == 1) {
        snprintf(out, cap, "「%s」が%sの口癖ですね。私も少しずつ、そう言うようになると思います。", e->s, p->address_user);
    } else if (pick == 2) {
        snprintf(out, cap, "「%s」、ですか。もう少し聞かせてください。", in);
    } else {
        snprintf(out, cap, "「%s」ですね。覚えておきます。", in);
    }
}

/* The user's voice: what they said next last time, or text generated from
 * their own n-grams, seeded with a word they used. */
static int mirrored_reply(struct brain *b, const char *in, char *out, size_t cap)
{
    int i = most_similar(b, in);
    if (i >= 0 && i + 1 < b->nutt - 1 && strcmp(b->utt[i + 1], in) != 0) {
        snprintf(out, cap, "%s", b->utt[i + 1]);
        return 1;
    }
    char kw[WORD_MAX];
    const char *k = known_keyword(b, in, kw, sizeof kw);
    for (int attempt = 0; attempt < 4; attempt++) {
        if (generate(b, attempt < 2 ? k : NULL, out, cap) && strcmp(out, in) != 0) return 1;
    }
    return 0;
}

void brain_reply(struct brain *b, const struct persona *p, const char *input, char *out, size_t cap)
{
    if (fixed_reply(b, p, input, out, cap)) return;
    int pct = brain_mirror_percent(b);
    if ((int)(rnd(b) % 100) < pct && mirrored_reply(b, input, out, cap)) return;
    polite_reply(b, p, input, out, cap);
}

void brain_self_model(const struct brain *b, const struct persona *p, char *out, size_t cap)
{
    if (b->nutt == 0) {
        snprintf(out, cap, "%sのことは、まだ何も知りません。これが最初の会話です。", p->address_user);
        return;
    }
    size_t o = 0;
    int w = snprintf(out + o, cap - o, "%sの言葉を %d 回聞きました。1 回あたり平均 %lu 文字。",
                     p->address_user, b->nutt, b->chars / (unsigned long)b->nutt);
    if (w < 0 || (size_t)w >= cap - o) return;
    o += (size_t)w;
    if (b->questions * 3 >= (unsigned long)b->nutt) w = snprintf(out + o, cap - o, "質問が多めです。");
    else if (b->exclaims * 3 >= (unsigned long)b->nutt) w = snprintf(out + o, cap - o, "「！」をよく使います。");
    else w = snprintf(out + o, cap - o, "落ち着いた言い方が多いです。");
    if (w < 0 || (size_t)w >= cap - o) return;
    o += (size_t)w;

    const struct word *e1 = top(b->endings, b->nendings, 0, NULL);
    const struct word *e2 = e1 ? top(b->endings, b->nendings, 0, e1->s) : NULL;
    if (e1) {
        w = snprintf(out + o, cap - o, "文末の口癖は「%s」(%u回)%s%s%s。", e1->s, e1->count,
                     e2 ? "、「" : "", e2 ? e2->s : "", e2 ? "」" : "");
        if (w < 0 || (size_t)w >= cap - o) return;
        o += (size_t)w;
    }
    const struct word *w1 = top(b->words, b->nwords, 1, NULL);
    const struct word *w2 = w1 ? top(b->words, b->nwords, 1, w1->s) : NULL;
    if (w1) {
        w = snprintf(out + o, cap - o, "よく出てくる言葉は「%s」%s%s%s。", w1->s,
                     w2 ? "と「" : "", w2 ? w2->s : "", w2 ? "」" : "");
        if (w < 0 || (size_t)w >= cap - o) return;
        o += (size_t)w;
    }
    snprintf(out + o, cap - o, "投影の度合いは %d%%(%d 回話すと %sの話し方になります)。",
             brain_mirror_percent(b), b->mirror_after, p->address_user);
}

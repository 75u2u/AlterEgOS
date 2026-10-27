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
#define WORDER     2            /* longest context of the English model, in words */
#define TOKS_MAX   64
#define GEN_WORDS  24

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

/* ---- English: word n-gram, context (order, words) -> next word counts.
 * English puts spaces between words, so the model can work on words where
 * the Japanese one has to work on characters. "" is the end-of-utterance
 * word; a context of "" words is the start. */

struct wnxt { char w[WORD_MAX]; unsigned count; };

struct wctx {
    int order;
    char key[WORDER][WORD_MAX];
    struct wnxt *nxt;
    unsigned n, cap, total;
    struct wctx *chain;
};

struct toks { char w[TOKS_MAX][WORD_MAX]; int n; };

#define FACTS_MAX 64
struct fact { char kind[8]; char text[96]; };   /* kind: like, dislike, about, note */

struct brain {
    struct ctx *bucket[BUCKETS];
    struct wctx *wbucket[BUCKETS];
    char **utt;                 /* the log, in order */
    int nutt, utt_cap;
    struct word words[WORDS_MAX];   /* content words: kanji / katakana / latin runs */
    int nwords;
    struct word endings[64];        /* last two code points of an utterance */
    int nendings;
    struct word endings_en[64];     /* last word of an English utterance */
    int nendings_en;
    unsigned long chars, questions, exclaims;
    int nutt_en;                    /* how many of the utterances were English */
    enum brain_lang lang;           /* language of the latest utterance */
    int stage_turns[3];
    int traits[T_COUNT];            /* -100..100, 0 at birth */
    struct fact facts[FACTS_MAX];
    int nfacts;
    char user_name[64], own_name[64];   /* learned in conversation; "" = not yet */
    uint64_t rng;
};

/* ---- language */

enum brain_lang brain_detect_lang(const char *s, enum brain_lang fallback)
{
    uint32_t cp;
    size_t k;
    int latin = 0;
    while ((k = utf8_decode(s, &cp)) > 0) {
        enum ctype t = utf8_class(cp);
        if (t == CT_HIRAGANA || t == CT_KATAKANA || t == CT_KANJI) return BRAIN_JA;
        if (t == CT_LATIN && cp != '\'') latin++;
        s += k;
    }
    return latin >= 2 ? BRAIN_EN : fallback;
}

enum brain_lang brain_lang(const struct brain *b) { return b->lang; }

static void lower_ascii(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }

/* Words that say nothing about what the user talks about. */
static int stopword(const char *w)
{
    static const char *const sw[] = {
        "i", "a", "an", "the", "to", "is", "am", "are", "was", "were", "be", "been", "it", "its", "it's",
        "you", "your", "me", "my", "we", "our", "he", "she", "they", "them", "his", "her", "and", "or",
        "but", "so", "of", "in", "on", "at", "for", "with", "that", "this", "these", "those", "do",
        "does", "did", "don't", "didn't", "have", "has", "had", "not", "no", "yes", "i'm", "i've",
        "i'd", "i'll", "can", "can't", "will", "would", "just", "too", "very", "really", "there",
        "what", "how", "why", "when", "who", "if", "as", "than", "then", "about", "up", "out", NULL };
    for (int i = 0; sw[i]; i++) if (strcmp(w, sw[i]) == 0) return 1;
    return 0;
}

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

static void word_count(struct word *tab, int *n, int cap, const char *s);

/* ---- English word n-gram */

static unsigned hash_wctx(int order, char (*key)[WORD_MAX])
{
    uint32_t h = 2166136261u ^ (uint32_t)(order + 7);
    for (int i = 0; i < order; i++)
        for (const char *p = key[i]; ; p++) { h ^= (unsigned char)*p; h *= 16777619u; if (!*p) break; }
    return h % BUCKETS;
}

static struct wctx *wctx_find(struct brain *b, int order, char (*key)[WORD_MAX], int create)
{
    unsigned h = hash_wctx(order, key);
    for (struct wctx *c = b->wbucket[h]; c; c = c->chain) {
        if (c->order != order) continue;
        int same = 1;
        for (int i = 0; i < order && same; i++) same = strcmp(c->key[i], key[i]) == 0;
        if (same) return c;
    }
    if (!create) return NULL;
    struct wctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->order = order;
    for (int i = 0; i < order; i++) snprintf(c->key[i], WORD_MAX, "%s", key[i]);
    c->chain = b->wbucket[h];
    b->wbucket[h] = c;
    return c;
}

static void wctx_add(struct wctx *c, const char *w)
{
    for (unsigned i = 0; i < c->n; i++)
        if (strcmp(c->nxt[i].w, w) == 0) { c->nxt[i].count++; c->total++; return; }
    if (c->n == c->cap) {
        unsigned cap = c->cap ? c->cap * 2 : 4;
        struct wnxt *p = realloc(c->nxt, cap * sizeof *p);
        if (!p) return;
        c->nxt = p;
        c->cap = cap;
    }
    snprintf(c->nxt[c->n].w, WORD_MAX, "%s", w);
    c->nxt[c->n].count = 1;
    c->n++;
    c->total++;
}

static void wngram_add(struct brain *b, char (*hist)[WORD_MAX], const char *w)
{
    for (int order = 1; order <= WORDER; order++) {
        struct wctx *c = wctx_find(b, order, hist + (WORDER - order), 1);
        if (c) wctx_add(c, w);
    }
}

static int wngram_next(struct brain *b, char (*hist)[WORD_MAX], char *out)
{
    for (int order = WORDER; order >= 1; order--) {
        struct wctx *c = wctx_find(b, order, hist + (WORDER - order), 0);
        if (!c || c->total == 0) continue;
        unsigned r = (unsigned)(rnd(b) % c->total);
        for (unsigned i = 0; i < c->n; i++) {
            if (r < c->nxt[i].count) { snprintf(out, WORD_MAX, "%s", c->nxt[i].w); return 1; }
            r -= c->nxt[i].count;
        }
    }
    return 0;
}

static void whist_push(char (*hist)[WORD_MAX], const char *w)
{
    memmove(hist[0], hist[1], (size_t)(WORDER - 1) * WORD_MAX);
    snprintf(hist[WORDER - 1], WORD_MAX, "%s", w);
}

static int is_punct_tok(const char *w) { return w[0] && !w[1] && strchr(".,!?;:", w[0]); }

/* Lower-case words, with . , ! ? ; : as words of their own. */
static void tokenize_en(const char *s, struct toks *t)
{
    char run[WORD_MAX];
    size_t rl = 0;
    uint32_t cp;
    size_t k;
    t->n = 0;
    while ((k = utf8_decode(s, &cp)) > 0) {
        enum ctype c = utf8_class(cp);
        if (c == CT_LATIN || c == CT_DIGIT) {
            if (rl + k < sizeof run) { memcpy(run + rl, s, k); rl += k; }
        } else {
            if (rl && t->n < TOKS_MAX) { run[rl] = '\0'; lower_ascii(run); snprintf(t->w[t->n++], WORD_MAX, "%s", run); }
            rl = 0;
            if (cp < 0x80 && strchr(".,!?;:", (int)cp) && t->n < TOKS_MAX) { t->w[t->n][0] = (char)cp; t->w[t->n][1] = '\0'; t->n++; }
        }
        s += k;
    }
    if (rl && t->n < TOKS_MAX) { run[rl] = '\0'; lower_ascii(run); snprintf(t->w[t->n++], WORD_MAX, "%s", run); }
}

static void learn_en(struct brain *b, const char *utterance)
{
    struct toks t;
    tokenize_en(utterance, &t);
    char hist[WORDER][WORD_MAX] = {{0}};
    for (int i = 0; i < t.n; i++) { wngram_add(b, hist, t.w[i]); whist_push(hist, t.w[i]); }
    wngram_add(b, hist, "");
    int last = t.n - 1;
    while (last >= 0 && is_punct_tok(t.w[last])) last--;
    if (last >= 1) word_count(b->endings_en, &b->nendings_en, 64, t.w[last]);   /* one word is a word, not a habit */
}

/* Generate English from the word model, optionally starting from `seed`. */
static int generate_en(struct brain *b, const char *seed, char *out, size_t cap)
{
    char hist[WORDER][WORD_MAX] = {{0}};
    size_t o = 0;
    int n = 0;
    if (seed) {
        whist_push(hist, seed);
        o = (size_t)snprintf(out, cap, "%s", seed);
        if (o >= cap) return 0;
        n = 1;
    }
    for (int i = 0; i < GEN_WORDS; i++) {
        char w[WORD_MAX];
        if (!wngram_next(b, hist, w) || !w[0]) break;
        int m = snprintf(out + o, cap - o, "%s%s", (o && !is_punct_tok(w)) ? " " : "", w);
        if (m < 0 || (size_t)m >= cap - o) break;
        o += (size_t)m;
        whist_push(hist, w);
        if (!is_punct_tok(w)) n++;
    }
    out[o] = '\0';
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
    for (char *q = out; *q; q++) if (q[0] == ' ' && q[1] == 'i' && (q[2] == ' ' || q[2] == '\'' || !q[2])) q[1] = 'I';
    return n >= 2;
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
    /* kana-only input (the screen's IME) has no kanji to split on: a short
     * hiragana run is taken as a word too */
    if (t == CT_HIRAGANA && utf8_count(run) >= 2 && utf8_count(run) <= 6) { word_count(b->words, &b->nwords, WORDS_MAX, run); return; }
    if (!is_content(t) || utf8_count(run) < 1) return;
    char w[WORD_MAX];
    snprintf(w, sizeof w, "%s", run);
    if (t == CT_LATIN) { lower_ascii(w); if (stopword(w)) return; }
    word_count(b->words, &b->nwords, WORDS_MAX, w);
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

static void absorb(struct brain *b, const char *utterance);

struct brain *brain_new(const int stage_turns[3])
{
    struct brain *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    for (int i = 0; i < 3; i++) b->stage_turns[i] = stage_turns && stage_turns[i] > 0 ? stage_turns[i] : (i == 0 ? 10 : i == 1 ? 30 : 60);
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
    for (int i = 0; i < BUCKETS; i++) {
        for (struct wctx *c = b->wbucket[i]; c;) { struct wctx *n = c->chain; free(c->nxt); free(c); c = n; }
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
    b->lang = brain_detect_lang(utterance, b->lang);
    if (b->lang == BRAIN_EN) { b->nutt_en++; learn_en(b, utterance); }

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
    if (b->lang == BRAIN_JA && ending_of(utterance, e, sizeof e)) word_count(b->endings, &b->nendings, 64, e);
    absorb(b, utterance);
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

int brain_stage(const struct brain *b)
{
    int s = 0;
    for (int i = 0; i < 3; i++) if (b->nutt >= b->stage_turns[i]) s = i + 1;
    return s;
}

int brain_trait(const struct brain *b, enum brain_trait t) { return b->traits[t]; }
int brain_memories_count(const struct brain *b) { return b->nfacts; }

const char *brain_user_name(const struct brain *b, const struct persona *p)
{
    if (b->user_name[0]) return b->user_name;
    return b->lang == BRAIN_EN ? p->address_user_en : p->address_user;
}

const char *brain_own_name(const struct brain *b, const struct persona *p)
{
    if (b->own_name[0]) return b->own_name;
    return p->name;
}

/* ---- public: answering */

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
    snprintf(g->w[g->n], WORD_MAX, "%s", run);
    if (t == CT_LATIN) lower_ascii(g->w[g->n]);
    g->n++;
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

/* `w` as a whole word of the lower-cased `lo`. */
static int has_word(const char *lo, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = lo; (p = strstr(p, w)) != NULL; p += n) {
        int left = p == lo || !((p[-1] >= 'a' && p[-1] <= 'z') || p[-1] == '\'');
        int right = !((p[n] >= 'a' && p[n] <= 'z') || p[n] == '\'');
        if (left && right) return 1;
    }
    return 0;
}

/* 1: status (growth, personality, names), 2: memories, 0: neither. */
static int query_kind(const char *line)
{
    if (has(line, "覚えてる") || has(line, "おぼえてる") || has(line, "覚えている") || has(line, "おぼえている") ||
        has(line, "記憶") || has(line, "きおく")) return 2;
    if (has(line, "ステータス") || has(line, "すてーたす") || has(line, "性格") || has(line, "せいかく") ||
        has(line, "どんな子") || has(line, "成長") || has(line, "せいちょう")) return 1;
    char lo[512];
    snprintf(lo, sizeof lo, "%s", line);
    lower_ascii(lo);
    if (has(lo, "what do you remember") || has(lo, "your memories") || has(lo, "what have you learned")) return 2;
    if (has_word(lo, "status") || has(lo, "your personality") || has(lo, "how are you growing") || has(lo, "how old are you")) return 1;
    return 0;
}

int brain_is_query(const char *line) { return query_kind(line) != 0; }


/* ---- personality and memories: what one utterance teaches */

struct kw { const char *s; int delta; };

static void bump(struct brain *b, enum brain_trait t, int d)
{
    int v = b->traits[t] + d;
    b->traits[t] = v > 100 ? 100 : v < -100 ? -100 : v;
}

static int kw_score(const char *s, const char *lo, const struct kw *list)
{
    int sum = 0;
    for (int i = 0; list[i].s; i++)
        if (has(s, list[i].s) || has(lo, list[i].s)) sum += list[i].delta;
    return sum;
}

static const struct kw cheer_kw[] = {
    {"！", 3}, {"!", 3}, {"笑", 3}, {"www", 3}, {"楽し", 4}, {"たのし", 4}, {"嬉し", 4}, {"うれし", 4},
    {"ありがと", 2}, {"やった", 4}, {"最高", 4}, {"いいね", 3}, {"happy", 4}, {"fun", 3}, {"great", 3},
    {"haha", 3}, {"lol", 3}, {"nice", 2}, {"awesome", 4},
    {"疲れ", -3}, {"つかれ", -3}, {"つらい", -4}, {"辛い", -4}, {"最悪", -4}, {"無理", -3}, {"むり", -3},
    {"悲し", -4}, {"かなし", -4}, {"tired", -3}, {"sad", -4}, {"ugh", -3}, {"awful", -4}, {"terrible", -4},
    {NULL, 0}
};
static const struct kw praise_kw[] = {                         /* said to the agent */
    {"えらい", 5}, {"偉い", 5}, {"いい子", 6}, {"かわいい", 5}, {"可愛い", 5}, {"すごい", 4}, {"賢い", 5},
    {"頭いい", 5}, {"上手", 4}, {"ありがとう", 2}, {"good girl", 5}, {"good job", 5}, {"well done", 5},
    {"smart", 4}, {"cute", 5}, {"clever", 4}, {"thank", 2},
    {"だめ", -5}, {"ダメ", -5}, {"駄目", -5}, {"違う", -3}, {"ちがう", -3}, {"やめて", -4}, {"うるさい", -5},
    {"ばか", -5}, {"バカ", -5}, {"馬鹿", -5}, {"wrong", -3}, {"stupid", -5}, {"shut up", -6}, {"bad girl", -5},
    {NULL, 0}
};
static const struct kw feeling_kw[] = {                        /* only when about the agent */
    {"大好き", 6}, {"だいすき", 6}, {"好き", 4}, {"すき", 4}, {"love", 5}, {"like", 3},
    {"嫌い", -5}, {"きらい", -5}, {"hate", -5}, {NULL, 0}
};

static int about_me(const char *s, const char *lo)
{
    return has(s, "君") || has(s, "きみ") || has(s, "あなた") || has(s, "お前") || has(s, "おまえ") ||
           has(s, "あんた") || has_word(lo, "you") || has_word(lo, "your");
}

/* Trim trailing punctuation and sentence endings so "ゆうやだよ。" becomes "ゆうや". */
static void strip_tail(char *s)
{
    static const char *const tails[] = {
        "。", "！", "!", "？", "?", "、", ",", ".", " ", "　",
        "だよ", "です", "なんだ", "なの", "だね", "だぞ", "だ", "よ", "ね", "にする", "にしよう", "にしたい", "に決めた",
        "と言います", "といいます", "って言います", "っていいます", "と申します", "ともうします", "って言うんだ", "っていうんだ",
        "ください", "くださいね", "ね", NULL };
    for (int again = 1; again;) {
        again = 0;
        size_t n = strlen(s);
        for (int i = 0; tails[i]; i++) {
            size_t t = strlen(tails[i]);
            if (n >= t && memcmp(s + n - t, tails[i], t) == 0) { s[n - t] = '\0'; again = 1; break; }
        }
    }
}

static const char *skip_subject(const char *s)
{
    static const char *const subj[] = { "私は", "僕は", "俺は", "わたしは", "ぼくは", "おれは", "自分は", "私、", "私も", "僕も", NULL };
    for (int i = 0; subj[i]; i++) {
        size_t n = strlen(subj[i]);
        if (strncmp(s, subj[i], n) == 0) return s + n;
    }
    return s;
}

static void remember(struct brain *b, const char *kind, const char *text)
{
    char t[96];
    snprintf(t, sizeof t, "%s", text);
    strip_tail(t);
    while (*t == ' ' || (unsigned char)*t == 0xE3) {            /* leading 、 or 　 */
        if (*t == ' ') memmove(t, t + 1, strlen(t));
        else if (strncmp(t, "、", 3) == 0 || strncmp(t, "　", 3) == 0 || strncmp(t, "：", 3) == 0) memmove(t, t + 3, strlen(t) - 2);
        else break;
    }
    if (*t == ':') memmove(t, t + 1, strlen(t));
    size_t cps = utf8_count(t);
    if (cps == 0 || cps > 40) return;
    for (int i = 0; i < b->nfacts; i++)
        if (strcmp(b->facts[i].kind, kind) == 0 && strcmp(b->facts[i].text, t) == 0) return;
    if (b->nfacts == FACTS_MAX) { memmove(b->facts, b->facts + 1, (FACTS_MAX - 1) * sizeof b->facts[0]); b->nfacts--; }
    snprintf(b->facts[b->nfacts].kind, sizeof b->facts[0].kind, "%s", kind);
    snprintf(b->facts[b->nfacts].text, sizeof b->facts[0].text, "%s", t);
    b->nfacts++;
}

static void set_name(char *dst, const char *src)
{
    char t[64];
    snprintf(t, sizeof t, "%s", src);
    strip_tail(t);
    while (*t == ' ') memmove(t, t + 1, strlen(t));
    size_t cps = utf8_count(t);
    if (cps == 0 || cps > 16) return;
    snprintf(dst, 64, "%s", t);
}

/* "X が好き" / "X が嫌い": the thing before the marker, after the subject. */
static void like_fact(struct brain *b, const char *u, const char *marker, const char *kind)
{
    const char *p = strstr(u, marker);
    if (!p) return;
    const char *end = p;
    if (end - u >= 3 && memcmp(end - 3, "大", 3) == 0) end -= 3;
    if (end - u >= 3 && (memcmp(end - 3, "が", 3) == 0 || memcmp(end - 3, "は", 3) == 0 || memcmp(end - 3, "も", 3) == 0)) end -= 3;
    else return;
    const char *start = skip_subject(u);
    if (end <= start) return;
    char x[96];
    snprintf(x, sizeof x, "%.*s", (int)(end - start), start);
    remember(b, kind, x);
}

static int starts(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }

static void facts_ja(struct brain *b, const char *u, int me)
{
    if (is_question(u)) return;                       /* "名前は？" asks, it does not tell */
    static const char *const my_name[] = { "私の名前は", "僕の名前は", "俺の名前は", "わたしの名前は", "ぼくの名前は", "おれの名前は", "自分の名前は", NULL };
    static const char *const your_name[] = { "君の名前は", "きみの名前は", "あなたの名前は", "お前の名前は", "おまえの名前は", "あんたの名前は", "名前は", NULL };
    for (int i = 0; my_name[i]; i++) if (starts(u, my_name[i])) { set_name(b->user_name, u + strlen(my_name[i])); return; }
    for (int i = 0; your_name[i]; i++) if (starts(u, your_name[i])) { set_name(b->own_name, u + strlen(your_name[i])); return; }
    const char *p;
    if ((p = strstr(u, "と呼んで")) || (p = strstr(u, "って呼んで"))) {       /* "ゆうやと呼んで" */
        if (!me) { char x[64]; snprintf(x, sizeof x, "%.*s", (int)(p - u), u); set_name(b->user_name, skip_subject(x)); }
        return;
    }
    if ((p = strstr(u, "と呼ぶ")) || (p = strstr(u, "って呼ぶ")) || (p = strstr(u, "と名付け")) || (p = strstr(u, "と名づけ"))) {
        char x[64]; snprintf(x, sizeof x, "%.*s", (int)(p - u), u); set_name(b->own_name, x);
        return;
    }
    if (starts(u, "覚えて") || starts(u, "おぼえて")) { remember(b, "note", u + (starts(u, "覚えて") ? 9 : 12)); return; }
    if ((p = strstr(u, "を覚えて")) || (p = strstr(u, "をおぼえて"))) { char x[96]; snprintf(x, sizeof x, "%.*s", (int)(p - u), u); remember(b, "note", x); return; }
    if (me) return;                                   /* "君が好き" is a feeling, not a fact */
    if (has(u, "好き") || has(u, "すき")) { like_fact(b, u, has(u, "好き") ? "好き" : "すき", "like"); return; }
    if (has(u, "嫌い") || has(u, "きらい") || has(u, "苦手")) { like_fact(b, u, has(u, "嫌い") ? "嫌い" : has(u, "きらい") ? "きらい" : "苦手", "dislike"); return; }
    const char *x = skip_subject(u);
    if (x != u && !has(u, "？") && !has(u, "?")) {    /* "私は学生です" */
        char t[96];
        snprintf(t, sizeof t, "%s", x);
        strip_tail(t);
        if (utf8_count(t) >= 1 && utf8_count(t) <= 30 && !has(t, "、") && utf8_count(t) <= 20) remember(b, "about", t);
    }
}

static void facts_en(struct brain *b, const char *u, const char *lo, int me)
{
    if (is_question(u)) return;
    static const struct { const char *pre; const char *kind; } pats[] = {
        {"my name is ", "uname"}, {"call me ", "uname"}, {"i am called ", "uname"},
        {"your name is ", "oname"}, {"i'll call you ", "oname"}, {"i will call you ", "oname"}, {"i name you ", "oname"},
        {"i like ", "like"}, {"i love ", "like"}, {"i really like ", "like"},
        {"i hate ", "dislike"}, {"i don't like ", "dislike"}, {"i dislike ", "dislike"},
        {"remember that ", "note"}, {"remember ", "note"}, {"don't forget ", "note"},
        {"i am a ", "about"}, {"i'm a ", "about"}, {"i am an ", "about"}, {"i'm an ", "about"}, {"i live in ", "about"}, {"i work as ", "about"},
        {NULL, NULL}
    };
    for (int i = 0; pats[i].pre; i++) {
        const char *p = strstr(lo, pats[i].pre);
        if (!p) continue;
        const char *x = u + (p - lo) + strlen(pats[i].pre);
        char t[96];
        snprintf(t, sizeof t, "%s", x);
        strip_tail(t);
        if (strcmp(pats[i].kind, "uname") == 0) set_name(b->user_name, t);
        else if (strcmp(pats[i].kind, "oname") == 0) set_name(b->own_name, t);
        else if ((strcmp(pats[i].kind, "like") == 0 || strcmp(pats[i].kind, "dislike") == 0) && me) return;
        else remember(b, pats[i].kind, t);
        return;
    }
}

static void absorb(struct brain *b, const char *u)
{
    char lo[512];
    snprintf(lo, sizeof lo, "%s", u);
    lower_ascii(lo);
    int me = about_me(u, lo);
    size_t cps = utf8_count(u);

    bump(b, T_CHEER, kw_score(u, lo, cheer_kw));
    int praise = kw_score(u, lo, praise_kw) + (me ? kw_score(u, lo, feeling_kw) : 0);
    bump(b, T_AFFECTION, praise);
    bump(b, T_CHEER, praise / 2);

    char t[512];
    snprintf(t, sizeof t, "%s", u);
    size_t n = strlen(t);
    while (n > 0 && (t[n - 1] == '!' || t[n - 1] == '?' || t[n - 1] == '.' || t[n - 1] == ' ')) t[--n] = '\0';
    for (int again = 1; again && n >= 3;) {
        again = 0;
        static const char *const punct[] = { "。", "！", "？", "、", NULL };
        for (int i = 0; punct[i]; i++) if (n >= 3 && memcmp(t + n - 3, punct[i], 3) == 0) { n -= 3; t[n] = '\0'; again = 1; }
    }
    static const char *const polite_end[] = { "です", "ます", "ました", "ません", "ください", "でしょう", "ですか", "ますか", NULL };
    static const char *const casual_end[] = { "だよ", "じゃん", "だろ", "だぞ", "だぜ", "かよ", "なあ", "だな", "ぞ", "ぜ", NULL };
    for (int i = 0; polite_end[i]; i++) { size_t m = strlen(polite_end[i]); if (n >= m && memcmp(t + n - m, polite_end[i], m) == 0) { bump(b, T_POLITE, 3); break; } }
    for (int i = 0; casual_end[i]; i++) { size_t m = strlen(casual_end[i]); if (n >= m && memcmp(t + n - m, casual_end[i], m) == 0) { bump(b, T_POLITE, -3); break; } }
    if (has(lo, "please") || has(lo, "thank you") || has(lo, "could you") || has(lo, "would you")) bump(b, T_POLITE, 2);
    if (has(u, "お前") || has(u, "てめえ") || has(lo, "damn") || has(lo, "shit") || has(lo, "fuck")) bump(b, T_POLITE, -4);

    if (is_question(u) || has(u, "なぜ") || has(u, "どうして") || has(u, "なんで") || has_word(lo, "why") || has_word(lo, "how")) bump(b, T_CURIOUS, 3);
    if (cps > 25) bump(b, T_TALK, 2);
    else if (cps < 6) bump(b, T_TALK, -2);

    if (b->lang == BRAIN_EN) facts_en(b, u, lo, me);
    else facts_ja(b, u, me);
}

/* ---- speaking */

static int style(const struct brain *b) { return b->traits[T_POLITE] > 15 ? 1 : b->traits[T_POLITE] < -15 ? 2 : 0; }   /* 0 plain, 1 polite, 2 casual */

/* Pick one of three phrasings by style, with printf arguments. */
#define SAY(b, out, cap, plain, polite, casual, ...) \
    snprintf(out, cap, style(b) == 1 ? polite : style(b) == 2 ? casual : plain, __VA_ARGS__)

static const char *longest_run(const char *input, char *buf, size_t cap)
{
    struct bag in = { .n = 0 };
    each_run(input, collect_run, &in);
    const char *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < in.n; i++) {
        uint32_t cp;
        utf8_decode(in.w[i], &cp);
        if (utf8_class(cp) == CT_DIGIT) continue;
        size_t l = utf8_count(in.w[i]);
        if (l > best_len) { best = in.w[i]; best_len = l; }
    }
    if (!best) return NULL;
    snprintf(buf, cap, "%s", best);
    return buf;
}

static const struct fact *random_fact(struct brain *b, const char *kind)
{
    int n = 0;
    for (int i = 0; i < b->nfacts; i++) if (!kind || strcmp(b->facts[i].kind, kind) == 0) n++;
    if (n == 0) return NULL;
    int pick = (int)(rnd(b) % (uint64_t)n);
    for (int i = 0; i < b->nfacts; i++) if (!kind || strcmp(b->facts[i].kind, kind) == 0) { if (pick-- == 0) return &b->facts[i]; }
    return NULL;
}

/* Lines that teach (names, "remember ...") are instructions, not things to say back. */
static int is_teaching(const char *u)
{
    char lo[512];
    snprintf(lo, sizeof lo, "%s", u);
    lower_ascii(lo);
    return has(u, "名前は") || has(u, "呼んで") || has(u, "呼ぶ") || has(u, "名付け") || has(u, "名づけ") ||
           has(u, "覚えて") || has(u, "おぼえて") || has(lo, "my name is") || has(lo, "your name is") ||
           has(lo, "call me") || has(lo, "call you") || has(lo, "remember");
}

/* Something the user said next last time, or text generated from their words. */
static int learned_reply(struct brain *b, const char *in, char *out, size_t cap, int max_cps)
{
    int i = most_similar(b, in);
    if (i >= 0 && i + 1 < b->nutt - 1 && strcmp(b->utt[i + 1], in) != 0 &&
        brain_detect_lang(b->utt[i + 1], b->lang) == b->lang && !brain_is_query(b->utt[i + 1]) && !is_teaching(b->utt[i + 1])) {
        snprintf(out, cap, "%s", b->utt[i + 1]);
        return 1;
    }
    char kw[WORD_MAX];
    const char *k = known_keyword(b, in, kw, sizeof kw);
    for (int attempt = 0; attempt < 4; attempt++) {
        const char *seed = attempt < 2 ? k : NULL;
        int ok = b->lang == BRAIN_EN ? generate_en(b, seed, out, cap) : generate(b, seed, out, cap);
        if (ok && strcasecmp(out, in) != 0 && (int)utf8_count(out) <= max_cps) return 1;
    }
    return 0;
}

/* Add the personality's colour to a finished sentence. */
static void colour(struct brain *b, char *out, size_t cap)
{
    size_t n = strlen(out);
    if (n == 0) return;
    int cheer = b->traits[T_CHEER];
    int ends_punct = n >= 3 && (memcmp(out + n - 3, "。", 3) == 0 || memcmp(out + n - 3, "！", 3) == 0 || memcmp(out + n - 3, "？", 3) == 0);
    if (!ends_punct && (out[n - 1] == '!' || out[n - 1] == '?' || out[n - 1] == '.')) ends_punct = 1;
    if (cheer > 25 && !ends_punct && n + 4 < cap && rnd(b) % 2 == 0) strcat(out, b->lang == BRAIN_EN ? "!" : "！");
    else if (cheer < -25 && n + 4 < cap && rnd(b) % 2 == 0) { memmove(out + 3, out, n + 1); memcpy(out, "…", 3); }
}

static int greeting_reply(struct brain *b, const struct persona *p, const char *in, const char *lo, char *out, size_t cap)
{
    const char *you = brain_user_name(b, p);
    int stage = brain_stage(b);
    if (b->lang == BRAIN_EN) {
        if (has(lo, "good morning")) snprintf(out, cap, stage == 0 ? "...morning?" : "Good morning, %s.", you);
        else if (has(lo, "good night")) snprintf(out, cap, stage == 0 ? "...night." : "Good night, %s.", you);
        else if (has_word(lo, "hello") || has_word(lo, "hi") || has_word(lo, "hey")) snprintf(out, cap, stage == 0 ? "...hi?" : "Hi, %s!", you);
        else if (has(lo, "thank")) snprintf(out, cap, stage == 0 ? "..." : stage == 1 ? "Thank... you?" : "You're welcome, %s.", you);
        else if (has(lo, "bye")) snprintf(out, cap, stage == 0 ? "...bye?" : "Bye, %s. Come back soon.", you);
        else return 0;
        return 1;
    }
    if (has(in, "おはよう")) {
        if (stage == 0) snprintf(out, cap, "……おはよう？");
        else SAY(b, out, cap, "おはよう、%s。", "おはようございます、%sさん。", "おはよ、%s！", you);
    } else if (has(in, "こんにちは")) {
        if (stage == 0) snprintf(out, cap, "……こんにちは？");
        else SAY(b, out, cap, "こんにちは、%s。", "こんにちは、%sさん。", "やっほー、%s！", you);
    } else if (has(in, "こんばんは")) {
        if (stage == 0) snprintf(out, cap, "……こんばんは？");
        else SAY(b, out, cap, "こんばんは、%s。", "こんばんは、%sさん。", "こんばんは〜、%s。", you);
    } else if (has(in, "おやすみ")) {
        if (stage == 0) snprintf(out, cap, "……おやすみ。");
        else SAY(b, out, cap, "おやすみ、%s。", "おやすみなさい、%sさん。", "おやすみー、%s。", you);
    } else if (has(in, "ありがと")) {
        if (stage == 0) snprintf(out, cap, "……？");
        else if (stage == 1) snprintf(out, cap, "ありがと……？");
        else SAY(b, out, cap, "どういたしまして。", "どういたしまして、%sさん。", "いいよいいよ、%s。", you);
    } else if (has(in, "ばいばい") || has(in, "バイバイ") || has(in, "さようなら") || has(in, "またね")) {
        if (stage == 0) snprintf(out, cap, "……ばいばい？");
        else SAY(b, out, cap, "またね、%s。", "いってらっしゃい、%sさん。", "ばいばい、%s！", you);
    } else {
        return 0;
    }
    return 1;
}

static void name_reply(struct brain *b, const struct persona *p, char *out, size_t cap)
{
    const char *you = brain_user_name(b, p);
    if (b->lang == BRAIN_EN) {
        if (b->own_name[0]) snprintf(out, cap, "My name is %s. You gave it to me, %s.", b->own_name, you);
        else snprintf(out, cap, "I don't have a name yet. Will you give me one? Say \"your name is ...\".");
        return;
    }
    if (b->own_name[0]) SAY(b, out, cap, "%s。%sがつけてくれた名前。", "%sです。%sさんがつけてくださった名前です。", "%s！%sがつけてくれたんだよ。", b->own_name, you);
    else if (brain_stage(b) == 0) snprintf(out, cap, "……？(まだ名前がありません。「君の名前は〇〇」でつけてください)");
    else SAY(b, out, cap, "まだ名前がない。%sがつけて？", "まだ名前がありません。%sさんがつけてくださいますか。", "名前、まだないんだ。%sがつけてよ。", you);
}

/* The word to echo: a name or fact just taught, else the longest run. */
static const char *echo_word(const struct brain *b, const char *in, char *buf, size_t cap)
{
    if (b->user_name[0] && has(in, b->user_name)) { snprintf(buf, cap, "%s", b->user_name); return buf; }
    if (b->own_name[0] && has(in, b->own_name)) { snprintf(buf, cap, "%s", b->own_name); return buf; }
    if (b->nfacts > 0 && has(in, b->facts[b->nfacts - 1].text)) { snprintf(buf, cap, "%s", b->facts[b->nfacts - 1].text); return buf; }
    return longest_run(in, buf, cap);
}

static void newborn_reply(struct brain *b, const char *in, char *out, size_t cap)
{
    char w[128];
    const char *k = echo_word(b, in, w, sizeof w);
    unsigned pick = (unsigned)(rnd(b) % 4);
    if (b->lang == BRAIN_EN) {
        if (k && pick < 3) snprintf(out, cap, pick == 0 ? "...%s?" : pick == 1 ? "%s..." : "...%s. %s?", k, k);
        else snprintf(out, cap, pick == 0 ? "..." : pick == 1 ? "(stares at you)" : "(tilts head)");
        return;
    }
    if (k && pick < 3) snprintf(out, cap, pick == 0 ? "……%s？" : pick == 1 ? "%s……" : "…%s、%s？", k, k);
    else snprintf(out, cap, pick == 0 ? "……" : pick == 1 ? "（じっと見ている）" : pick == 2 ? "（首をかしげる）" : "……？");
}

static void babbling_reply(struct brain *b, const struct persona *p, const char *in, char *out, size_t cap)
{
    char kw[WORD_MAX], w[128];
    const char *k = known_keyword(b, in, kw, sizeof kw);
    const char *any = echo_word(b, in, w, sizeof w);
    const char *you = brain_user_name(b, p);
    unsigned pick = (unsigned)(rnd(b) % 3);
    int curious = b->traits[T_CURIOUS] > 10;
    if (b->lang == BRAIN_EN) {
        if (k && pick == 0) snprintf(out, cap, "%s! I know %s.", k, k);
        else if (k && pick == 1 && learned_reply(b, in, out, cap, 6)) return;
        else if (any && curious) snprintf(out, cap, "\"%s\"... what is that, %s?", any, you);
        else if (any) snprintf(out, cap, "%s... %s.", any, any);
        else snprintf(out, cap, pick == 0 ? "Mm." : pick == 1 ? "...yes?" : "Mm-hm.");
        return;
    }
    if (k && pick == 0) SAY(b, out, cap, "%s！%s、知ってる。", "%s。%s、知っています。", "%s！%s、知ってる！", k, k);
    else if (k && pick == 1 && learned_reply(b, in, out, cap, 8)) return;
    else if (any && curious) SAY(b, out, cap, "「%s」って、なに？", "「%s」って、なんですか？", "「%s」ってなに、%s？", any, you);
    else if (any) snprintf(out, cap, "%s……%s。", any, any);
    else snprintf(out, cap, pick == 0 ? "うん。" : pick == 1 ? "……うん？" : "ん。");
}

static void chatty_reply(struct brain *b, const struct persona *p, const char *in, char *out, size_t cap)
{
    const char *you = brain_user_name(b, p);
    int grown = brain_stage(b) >= 3;
    unsigned pick = (unsigned)(rnd(b) % 5);
    const struct fact *f;
    if (pick == 0 && (f = random_fact(b, "like")) != NULL && !has(in, f->text)) {
        if (b->lang == BRAIN_EN) snprintf(out, cap, "By the way, you said you like %s, %s.", f->text, you);
        else SAY(b, out, cap, "そういえば、%sが好きって言ってたよね、%s。", "そういえば、%sがお好きなんですよね、%sさん。", "てか%s好きなんでしょ、%s！", f->text, you);
        return;
    }
    if (is_question(in)) {
        if (learned_reply(b, in, out, cap, 60)) { colour(b, out, cap); return; }
        if (b->lang == BRAIN_EN) snprintf(out, cap, b->traits[T_CURIOUS] > 10 ? "I don't know yet. What do you think, %s?" : "I don't know that yet, %s.", you);
        else if (b->traits[T_CURIOUS] > 10) SAY(b, out, cap, "まだわからない。%sはどう思う？", "まだわかりません。%sさんはどう思いますか？", "わかんない！%sはどう思うの？", you);
        else SAY(b, out, cap, "まだわからない。%.0s", "まだわかりません。%.0s", "わかんないなあ。%.0s", you);
        return;
    }
    if (learned_reply(b, in, out, cap, grown ? 80 : 40)) {
        colour(b, out, cap);
        if (grown && b->traits[T_TALK] > 15 && b->nfacts > 0 && rnd(b) % 3 == 0 && (f = random_fact(b, NULL)) != NULL) {
            size_t n = strlen(out);
            if (b->lang == BRAIN_EN) snprintf(out + n, cap - n, " I remember: %s.", f->text);
            else snprintf(out + n, cap - n, " %sのこと、覚えてるよ。", f->text);
        }
        return;
    }
    char w[WORD_MAX];
    const char *any = longest_run(in, w, sizeof w);
    if (b->lang == BRAIN_EN) {
        if (any) snprintf(out, cap, pick < 2 ? "\"%s\"... tell me more, %s." : "%s. I'll remember that.", any, you);
        else snprintf(out, cap, "Mm-hm.");
        return;
    }
    if (any) SAY(b, out, cap, "%sのこと、もっと教えて。", "%sのこと、もっと教えてください。", "%s？もっと聞かせて、%s！", any, you);
    else SAY(b, out, cap, "うん、聞いてる。%.0s", "はい、聞いています。%.0s", "うんうん。%.0s", you);
    colour(b, out, cap);
}

/* Praise and scolding get an answer of their own: this is how it is raised. */
static int feedback_reply(struct brain *b, const struct persona *p, const char *in, const char *lo, char *out, size_t cap)
{
    int me = about_me(in, lo);
    int score = kw_score(in, lo, praise_kw) + (me ? kw_score(in, lo, feeling_kw) : 0);
    if (score == 0) return 0;
    const char *you = brain_user_name(b, p);
    int stage = brain_stage(b);
    unsigned pick = (unsigned)(rnd(b) % 2);
    if (b->lang == BRAIN_EN) {
        if (score > 0) snprintf(out, cap, stage == 0 ? "...! (happy)" : stage == 1 ? "Hehe. %s..." : pick ? "Hehe, thank you, %s. I like you too." : "That makes me happy, %s!", you);
        else snprintf(out, cap, stage == 0 ? "... (shrinks)" : stage == 1 ? "...sorry, %s." : pick ? "I'm sorry, %s. I'll do better." : "...did I do something wrong, %s?", you);
        return 1;
    }
    if (score > 0) {
        if (stage == 0) snprintf(out, cap, "……！（うれしそう）");
        else if (stage == 1) snprintf(out, cap, "えへへ。%s……", you);
        else if (pick) SAY(b, out, cap, "えへへ。ありがとう、%s。私も%sが好き。", "ありがとうございます、%sさん。私も%sさんのことが好きです。", "えへへ、%s！私も%s、大好き！", you, you);
        else SAY(b, out, cap, "うれしい。%sにそう言ってもらえると。", "うれしいです。%sさんにそう言っていただけると。", "やったー！%s、うれしい！", you);
    } else {
        if (stage == 0) snprintf(out, cap, "……（しゅんとしている）");
        else if (stage == 1) snprintf(out, cap, "……ごめん、%s。", you);
        else if (pick) SAY(b, out, cap, "ごめんなさい、%s。気をつける。", "申し訳ありません、%sさん。気をつけます。", "ごめん、%s……。", you);
        else SAY(b, out, cap, "……何か、まちがえた？%s。", "……何か、間違えましたか？%sさん。", "え、なんか変だった？%s。", you);
    }
    return 1;
}

void brain_reply(struct brain *b, const struct persona *p, const char *input, char *out, size_t cap)
{
    b->lang = brain_detect_lang(input, b->lang);     /* answer in the language spoken to */
    int q = query_kind(input);
    if (q == 1) { brain_status(b, p, out, cap); return; }
    if (q == 2) { brain_memories(b, p, out, cap); return; }
    char lo[512];
    snprintf(lo, sizeof lo, "%s", input);
    lower_ascii(lo);
    if (greeting_reply(b, p, input, lo, out, cap)) return;
    if (feedback_reply(b, p, input, lo, out, cap)) return;
    int asks_name = (has(input, "名前") || has(lo, "your name") || has(lo, "who are you")) && is_question(input);
    if (asks_name) { name_reply(b, p, out, cap); return; }
    switch (brain_stage(b)) {
    case 0: newborn_reply(b, input, out, cap); break;
    case 1: babbling_reply(b, p, input, out, cap); break;
    default: chatty_reply(b, p, input, out, cap); break;
    }
}

static const char *stage_name(const struct brain *b)
{
    static const char *const ja[] = { "生まれたて", "片言", "おしゃべり", "一人前" };
    static const char *const en[] = { "newborn", "babbling", "chatty", "grown up" };
    return (b->lang == BRAIN_EN ? en : ja)[brain_stage(b)];
}

void brain_status(const struct brain *b, const struct persona *p, char *out, size_t cap)
{
    const char *you = brain_user_name(b, p);
    int stage = brain_stage(b);
    int next = stage < 3 ? b->stage_turns[stage] - b->nutt : 0;
    struct { enum brain_trait t; const char *ja_hi, *ja_lo, *en_hi, *en_lo; } adj[] = {
        { T_CHEER, "明るい", "おとなしい", "cheerful", "quiet" },
        { T_POLITE, "ていねい", "くだけた", "polite", "casual" },
        { T_CURIOUS, "好奇心旺盛", "のんびり", "curious", "easygoing" },
        { T_TALK, "おしゃべり", "口数が少ない", "chatty", "a person of few words" },
        { T_AFFECTION, "甘えん坊", "すねている", "affectionate", "sulky" },
    };
    char traits[256] = "";
    for (size_t i = 0; i < sizeof adj / sizeof adj[0]; i++) {
        int v = b->traits[adj[i].t];
        if (v <= 20 && v >= -20) continue;
        const char *word = b->lang == BRAIN_EN ? (v > 0 ? adj[i].en_hi : adj[i].en_lo) : (v > 0 ? adj[i].ja_hi : adj[i].ja_lo);
        size_t n = strlen(traits);
        snprintf(traits + n, sizeof traits - n, "%s%s", n ? (b->lang == BRAIN_EN ? ", " : "、") : "", word);
    }
    if (b->lang == BRAIN_EN) {
        snprintf(out, cap, "Name: %s. Your name: %s. Growth: %s (%d conversations%s%d%s). Personality: %s. Words learned: %d, memories: %d (ask \"what do you remember\").",
                 b->own_name[0] ? b->own_name : "none yet (say \"your name is ...\")",
                 b->user_name[0] ? b->user_name : "not told yet (say \"my name is ...\")",
                 stage_name(b), b->nutt, stage < 3 ? ", " : "", next, stage < 3 ? " more to the next stage" : "",
                 traits[0] ? traits : "nothing has sprouted yet", b->nwords, b->nfacts);
        return;
    }
    snprintf(out, cap, "名前: %s。%sの名前: %s。成長: %s(%d 回話した%s%d%s)。性格: %s。覚えた言葉 %d 語、覚えていること %d 件(「何を覚えてる？」で聞けます)。",
             b->own_name[0] ? b->own_name : "まだない(「君の名前は〇〇」でつけてください)",
             you, b->user_name[0] ? b->user_name : "まだ聞いていない(「私の名前は〇〇」で教えてください)",
             stage_name(b), b->nutt, stage < 3 ? "、あと " : "", next, stage < 3 ? " 回で次の段階" : "",
             traits[0] ? traits : "まだ芽生えていない", b->nwords, b->nfacts);
}

void brain_memories(const struct brain *b, const struct persona *p, char *out, size_t cap)
{
    const char *you = brain_user_name(b, p);
    if (b->nfacts == 0) {
        if (b->lang == BRAIN_EN) snprintf(out, cap, "I don't remember anything yet. Tell me what you like, who you are, or say \"remember ...\".");
        else snprintf(out, cap, "まだ何も覚えていません。好きなもの、%sのこと、「覚えて〜」で教えてください。", you);
        return;
    }
    size_t o = 0;
    for (int i = 0; i < b->nfacts; i++) {
        const struct fact *f = &b->facts[i];
        const char *label;
        if (b->lang == BRAIN_EN)
            label = strcmp(f->kind, "like") == 0 ? "likes" : strcmp(f->kind, "dislike") == 0 ? "dislikes" : strcmp(f->kind, "about") == 0 ? "about you" : "note";
        else
            label = strcmp(f->kind, "like") == 0 ? "好き" : strcmp(f->kind, "dislike") == 0 ? "嫌い" : strcmp(f->kind, "about") == 0 ? "あなたのこと" : "メモ";
        int w = snprintf(out + o, cap - o, "%s%s: %s", i ? (b->lang == BRAIN_EN ? " / " : " ／ ") : "", label, f->text);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
}

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
    int mirror_after;
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

int brain_is_query(const char *line)
{
    if (has(line, "わたし像")) return 1;
    char lo[512];
    snprintf(lo, sizeof lo, "%s", line);
    lower_ascii(lo);
    return has(lo, "know about me") || has(lo, "who am i") || has(lo, "what have you learned") ||
           has(lo, "what did you learn") || has(lo, "remember about me");
}

static void self_model_en(const struct brain *b, const struct persona *p, char *out, size_t cap);

static int fixed_reply_en(const struct brain *b, const struct persona *p, const char *in, char *out, size_t cap)
{
    char lo[512];
    snprintf(lo, sizeof lo, "%s", in);
    lower_ascii(lo);
    const char *u = p->address_user_en;
    if (has(lo, "good morning"))   { snprintf(out, cap, "Good morning, %s. Let's make today a good one.", u); return 1; }
    if (has(lo, "good night"))     { snprintf(out, cap, "Good night. I will keep what you told me, %s.", u); return 1; }
    if (has(lo, "good evening"))   { snprintf(out, cap, "Good evening, %s. How was your day?", u); return 1; }
    if (has_word(lo, "hello") || has_word(lo, "hi") || has_word(lo, "hey")) { snprintf(out, cap, "Hello, %s.", u); return 1; }
    if (has(lo, "thank"))          { snprintf(out, cap, "You're welcome. I'm glad I could help you, %s.", u); return 1; }
    if (has(lo, "bye"))            { snprintf(out, cap, "See you later, %s.", u); return 1; }
    if (has(lo, "your name") || has(lo, "who are you") || has(lo, "what are you")) {
        snprintf(out, cap, "I am %s, %s's other self: this OS itself.", p->name, u); return 1;
    }
    if (brain_is_query(in)) { self_model_en(b, p, out, cap); return 1; }
    if (has(lo, "what can you do") || has(lo, "what do you do")) {
        snprintf(out, cap, "For now I listen and learn the way %s talks. The more you talk, the more my replies sound like you.", u);
        return 1;
    }
    return 0;
}

/* A few things AlterEgo answers in its own words whatever the mirror ratio. */
static int fixed_reply(const struct brain *b, const struct persona *p, const char *in, char *out, size_t cap)
{
    if (b->lang == BRAIN_EN) return fixed_reply_en(b, p, in, out, cap);
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
    unsigned pick = (unsigned)(rnd(b) % 4);
    if (b->lang == BRAIN_EN) {
        const char *u = p->address_user_en;
        const struct word *e = top(b->endings_en, b->nendings_en, 0, NULL);
        if (is_question(in)) {
            snprintf(out, cap, "What would %s say to that? We have only talked %d times, so let me borrow your own words.", u, b->nutt);
        } else if (k && pick == 0) {
            snprintf(out, cap, "You mentioned \"%s\" before. Is it on your mind?", k);
        } else if (e && pick == 1) {
            snprintf(out, cap, "You often end with \"%s\", %s. I think I will start saying it too.", e->s, u);
        } else if (pick == 2) {
            snprintf(out, cap, "\"%s\"? Tell me more.", in);
        } else {
            snprintf(out, cap, "\"%s\". I will remember that.", in);
        }
        return;
    }
    const struct word *e = top(b->endings, b->nendings, 0, NULL);
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
    if (i >= 0 && i + 1 < b->nutt - 1 && strcmp(b->utt[i + 1], in) != 0 &&
        brain_detect_lang(b->utt[i + 1], b->lang) == b->lang) {
        snprintf(out, cap, "%s", b->utt[i + 1]);
        return 1;
    }
    char kw[WORD_MAX];
    const char *k = known_keyword(b, in, kw, sizeof kw);
    for (int attempt = 0; attempt < 4; attempt++) {
        const char *seed = attempt < 2 ? k : NULL;
        int ok = b->lang == BRAIN_EN ? generate_en(b, seed, out, cap) : generate(b, seed, out, cap);
        if (ok && strcasecmp(out, in) != 0) return 1;
    }
    return 0;
}

void brain_reply(struct brain *b, const struct persona *p, const char *input, char *out, size_t cap)
{
    b->lang = brain_detect_lang(input, b->lang);     /* answer in the language spoken to */
    if (fixed_reply(b, p, input, out, cap)) return;
    int pct = brain_mirror_percent(b);
    if ((int)(rnd(b) % 100) < pct && mirrored_reply(b, input, out, cap)) return;
    polite_reply(b, p, input, out, cap);
}

static void self_model_en(const struct brain *b, const struct persona *p, char *out, size_t cap)
{
    const char *u = p->address_user_en;
    if (b->nutt == 0) {
        snprintf(out, cap, "I know nothing about %s yet. This is our first conversation.", u);
        return;
    }
    size_t o = 0;
    int w = snprintf(out + o, cap - o, "I have heard %s speak %d times, %d of them in English, about %lu characters each. ",
                     u, b->nutt, b->nutt_en, b->chars / (unsigned long)b->nutt);
    if (w < 0 || (size_t)w >= cap - o) return;
    o += (size_t)w;
    if (b->questions * 3 >= (unsigned long)b->nutt) w = snprintf(out + o, cap - o, "You ask a lot of questions. ");
    else if (b->exclaims * 3 >= (unsigned long)b->nutt) w = snprintf(out + o, cap - o, "You use \"!\" a lot. ");
    else w = snprintf(out + o, cap - o, "You mostly speak calmly. ");
    if (w < 0 || (size_t)w >= cap - o) return;
    o += (size_t)w;

    const struct word *e1 = top(b->endings_en, b->nendings_en, 0, NULL);
    const struct word *e2 = e1 ? top(b->endings_en, b->nendings_en, 0, e1->s) : NULL;
    if (e1) {
        w = snprintf(out + o, cap - o, "Your habit is to end with \"%s\" (%u times)%s%s%s. ", e1->s, e1->count,
                     e2 ? ", also \"" : "", e2 ? e2->s : "", e2 ? "\"" : "");
        if (w < 0 || (size_t)w >= cap - o) return;
        o += (size_t)w;
    }
    const struct word *w1 = top(b->words, b->nwords, 1, NULL);
    const struct word *w2 = w1 ? top(b->words, b->nwords, 1, w1->s) : NULL;
    if (w1) {
        w = snprintf(out + o, cap - o, "Words you often use: \"%s\"%s%s%s. ", w1->s,
                     w2 ? " and \"" : "", w2 ? w2->s : "", w2 ? "\"" : "");
        if (w < 0 || (size_t)w >= cap - o) return;
        o += (size_t)w;
    }
    snprintf(out + o, cap - o, "Projection is at %d%% (after %d utterances I will talk the way %s does).",
             brain_mirror_percent(b), b->mirror_after, u);
}

void brain_self_model(const struct brain *b, const struct persona *p, char *out, size_t cap)
{
    if (b->lang == BRAIN_EN) { self_model_en(b, p, out, cap); return; }
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

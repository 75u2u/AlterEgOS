#define _GNU_SOURCE
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "brain.h"
#include "llm.h"
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
struct fact { char kind[8]; char text[96]; int kept; };   /* kind: like, dislike, about, note, def, mine; kept: not in the log, saved in bond.txt */

/* The questions AlterEgo asks on its own (see "closeness" below). */
enum { Q_YOUR_NAME, Q_MY_NAME, Q_FOOD, Q_COLOR, Q_HOBBY, Q_LIVE, Q_LIKE_ME, Q_BIRTHDAY, Q_DREAM, Q_TODAY, Q_FEEL, Q_FUN, Q_COUNT };

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
    char pending[64];               /* a word it asked about ("Xって何？"): the next line defines it */
    char asked[8][64];              /* the last words it was asked about: "Xは…" later still defines them */
    int nasked;
    int just_defined;               /* the latest utterance was that definition */
    int body;                       /* the machine's condition, 0 fine .. 3 struggling (body.c) */
    /* closeness to the user (bond.c part of the mind, persisted in bond.txt) */
    int intimacy;                   /* 0..100: grows with every conversation, praise, answers, being told things */
    int mood;                       /* -100..100, short-lived: praise, scolding, loneliness; fades every turn */
    unsigned asked_q;               /* bit per question id: asked already */
    int pending_q;                  /* the question waiting for its answer, or -1 */
    int ask_now;                    /* the question to append to the next reply, or -1 */
    int answered;                   /* what the latest utterance was: 1 an answer, 2/3 yes/no to 「私のこと好き？」, 4 a name given on request */
    char answer[96];
    char adopted[96];               /* a like of the user it just made its own */
    int sessions;                   /* boots with this memory, this one included */
    long absence_s;                 /* seconds since it was last talked to, at this boot */
    int idle_count;                 /* times it spoke up into silence */
    int last_q;                     /* nutt when it last asked something */
    int replaying;                  /* rebuilding from the log: no questions, bond.txt has the answers */
    /* its own language model (llm.c), trained on the conversation */
    struct llm *llm;
    int llm_said;                   /* replies that came out of the model, this session */
    char expr[16];                  /* what it felt while composing the latest reply, for its face ("" = nothing special) */
    uint64_t rng;
};

void brain_set_body(struct brain *b, int level) { b->body = level < 0 ? 0 : level > 3 ? 3 : level; }

static const char *body_word(const struct brain *b)
{
    static const char *const ja[] = { "元気", "ちょっと重い", "疲れている", "苦しい" };
    static const char *const en[] = { "fine", "a bit heavy", "tired", "struggling" };
    return (b->lang == BRAIN_EN ? en : ja)[b->body];
}

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
static int is_teaching(const char *u);

/* The language model learns the conversation as pairs: what was said, and
 * what the user said next. Lines that teach (names, "remember") are not
 * things to say back. While talking, a new line is studied right away (two
 * steps, mostly on it); the rest of the studying happens in the pauses
 * (brain_review). */
static void llm_learn(struct brain *b, const char *utterance)
{
    if (is_teaching(utterance)) return;
    const char *prev = b->nutt >= 2 ? b->utt[b->nutt - 2] : "";
    llm_add_example(b->llm, prev, utterance);
    if (!b->replaying) { llm_step(b->llm, 0.8); llm_step(b->llm, 0.5); }
}

struct brain *brain_new(const int stage_turns[3])
{
    struct brain *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    for (int i = 0; i < 3; i++) b->stage_turns[i] = stage_turns && stage_turns[i] > 0 ? stage_turns[i] : (i == 0 ? 10 : i == 1 ? 30 : 60);
    b->rng = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ 0x9E3779B97F4A7C15ull;
    if (!b->rng) b->rng = 1;
    b->pending_q = b->ask_now = -1;
    b->sessions = 1;
    b->llm = llm_new(b->rng);
    if (!b->llm) { free(b); return NULL; }
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
    llm_free(b->llm);
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
    llm_learn(b, utterance);
}

/* ---- the language model, seen from outside */

int brain_review(struct brain *b, int budget_ms)
{
    if (!llm_wants_review(b->llm)) return 0;
    return llm_train(b->llm, budget_ms);
}

int brain_wants_review(const struct brain *b) { return llm_wants_review(b->llm); }

int brain_model_load(struct brain *b, const unsigned char *data, size_t len)
{
    return llm_load(b->llm, data, len);
}

unsigned char *brain_model_save(struct brain *b, size_t *len, int force)
{
    if (!force && llm_unsaved(b->llm) < 300) return NULL;
    if (llm_steps(b->llm) == 0) return NULL;
    return llm_save(b->llm, len);
}

int brain_speaks_itself(const struct brain *b) { return llm_ready(b->llm); }

/* How the model should speak now: its personality sets the temperature
 * (curiosity and cheer loosen it, politeness tightens it), its stage and
 * talkativeness the length. */
static double llm_temperature(const struct brain *b)
{
    double t = 0.75;
    if (b->traits[T_CURIOUS] > 10) t += 0.2;
    if (b->traits[T_CHEER] > 25) t += 0.1;
    if (b->traits[T_POLITE] > 15) t -= 0.1;
    if (b->mood > 30) t += 0.05;
    return t < 0.5 ? 0.5 : t > 1.1 ? 1.1 : t;
}

static int llm_max_tokens(const struct brain *b)
{
    int n = brain_stage(b) <= 1 ? 10 : brain_stage(b) == 2 ? 24 : 40;
    if (b->traits[T_TALK] > 15) n = n * 3 / 2;
    if (b->traits[T_TALK] < -15) n = n * 2 / 3;
    return n;
}

/* A reply from the model: what the user would say next, in the user's
 * words; 0 when it is not ready or what came out is not worth saying. */
static int llm_reply(struct brain *b, const char *in, char *out, size_t cap, int max_cps)
{
    if (!llm_ready(b->llm)) return 0;
    int max_tokens = llm_max_tokens(b);
    if (max_tokens > max_cps) max_tokens = max_cps;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!llm_generate(b->llm, in, llm_temperature(b), max_tokens, out, cap)) continue;
        if (utf8_count(out) < 2 || strcasecmp(out, in) == 0) continue;
        if (brain_detect_lang(out, b->lang) != b->lang || brain_is_query(out) || is_teaching(out)) continue;
        b->llm_said++;
        return 1;
    }
    out[0] = '\0';
    return 0;
}

void brain_load_log(struct brain *b, const char *text)
{
    const char *p = text;
    b->replaying = 1;
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
    b->replaying = 0;
    b->answered = 0;
    b->ask_now = -1;
    b->adopted[0] = '\0';
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

static void remember(struct brain *b, const char *kind, const char *text);
void brain_remember_note(struct brain *b, const char *text) { remember(b, "note", text); }

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
    b->facts[b->nfacts].kept = 0;
    b->nfacts++;
}

/* A fact that cannot be rebuilt from the utterance log (an answer to one of
 * its own questions): marked so bond.txt keeps it. */
static void remember_kept(struct brain *b, const char *kind, const char *text)
{
    int before = b->nfacts;
    remember(b, kind, text);
    if (b->nfacts > before) b->facts[b->nfacts - 1].kept = 1;
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

/* "Xって何？" / "what is X": the word asked about, or 0. */
static int ask_what(const char *u, const char *lo, char *out, size_t cap)
{
    static const char *const ja_tails[] = { "って何", "ってなに", "とは何", "とはなに", "とは", "って", "って？", "ってなんですか", NULL };
    const char *p = NULL;
    for (int i = 0; ja_tails[i] && !p; i++) {
        const char *q = strstr(u, ja_tails[i]);
        if (q && q != u) {
            /* the tail must end the sentence apart from "？" / "ですか" / "なの" */
            const char *rest = q + strlen(ja_tails[i]);
            while (*rest) {
                if (starts(rest, "？") || starts(rest, "?")) { rest += starts(rest, "？") ? 3 : 1; continue; }
                if (starts(rest, "ですか") || starts(rest, "なの") || starts(rest, "だ") || starts(rest, "なん")) { rest += strlen(starts(rest, "ですか") ? "ですか" : starts(rest, "なの") ? "なの" : starts(rest, "なん") ? "なん" : "だ"); continue; }
                if (*rest == ' ' || starts(rest, "。") || starts(rest, "、")) { rest += *rest == ' ' ? 1 : 3; continue; }
                break;
            }
            if (*rest == '\0' && is_question(u)) p = q;
        }
    }
    if (p) {
        size_t n = (size_t)(p - u);
        if (n == 0 || n >= cap) return 0;
        memcpy(out, u, n);
        out[n] = '\0';
        strip_tail(out);
        return utf8_count(out) >= 1 && utf8_count(out) <= 20;
    }
    {   /* "btrfs?": one Latin word and a question mark */
        size_t n = strlen(u);
        if (n >= 2 && n < cap && u[n - 1] == '?' && strchr(u, ' ') == NULL) {
            int latin = 1;
            for (size_t i = 0; i + 1 < n; i++) if (!(isalnum((unsigned char)u[i]) || u[i] == '-' || u[i] == '_')) latin = 0;
            if (latin) { memcpy(out, u, n - 1); out[n - 1] = '\0'; return 1; }
        }
    }
    static const char *const en_heads[] = { "what is a ", "what is an ", "what is ", "what's a ", "what's an ", "what's ", "what are ", "what does ", NULL };
    for (int i = 0; en_heads[i]; i++) {
        if (!starts(lo, en_heads[i])) continue;
        const char *x = u + strlen(en_heads[i]);
        snprintf(out, cap, "%s", x);
        strip_tail(out);
        char *sp = strstr(out, " mean");
        if (sp) *sp = '\0';
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '?' || out[n - 1] == ' ')) out[--n] = '\0';
        if (has(out, "you") || has(out, "your") || has(out, "time") || has(out, "date")) return 0;   /* "what's your name", "what time" are others' */
        return utf8_count(out) >= 1 && utf8_count(out) <= 20 && strchr(out, ' ') == NULL;
    }
    return 0;
}

/* The definition stored for `x`, or NULL. */
static const char *find_def(const struct brain *b, const char *x)
{
    size_t n = strlen(x);
    for (int i = b->nfacts - 1; i >= 0; i--)
        if (strcmp(b->facts[i].kind, "def") == 0 && strncmp(b->facts[i].text, x, n) == 0 && strncmp(b->facts[i].text + n, ": ", 2) == 0)
            return b->facts[i].text + n + 2;
    return NULL;
}

/* The line after "Xって何？" explains X: keep "X: explanation". */
static int define(struct brain *b, const char *x, const char *u, const char *lo, int allow_unheaded)
{
    const char *body = u;
    int headed = 0;
    char head[96];
    static const char *const ja_joins[] = { "は", "って", "とは", "というのは", "っていうのは", NULL };
    for (int i = 0; ja_joins[i]; i++) {
        snprintf(head, sizeof head, "%.60s%s", x, ja_joins[i]);
        if (starts(u, head)) { body = u + strlen(head); headed = 1; break; }
    }
    if (!headed) {
        /* "a kernel is the core of an OS": an article may precede the word */
        size_t art = starts(lo, "a ") ? 2 : starts(lo, "an ") ? 3 : starts(lo, "the ") ? 4 : 0;
        static const char *const en_joins[] = { " is ", " means ", " are ", NULL };
        for (int i = 0; en_joins[i]; i++) {
            char lx[96];
            snprintf(lx, sizeof lx, "%s", x);
            lower_ascii(lx);
            snprintf(head, sizeof head, "%.60s%s", lx, en_joins[i]);
            if (starts(lo + art, head)) { body = u + art + strlen(head); headed = 1; break; }
        }
    }
    if (!headed && b->lang == BRAIN_EN) {
        static const char *const bare[] = { "it is ", "it's ", "that is ", "that's ", NULL };
        for (int i = 0; bare[i]; i++) if (starts(lo, bare[i])) { body = u + strlen(bare[i]); headed = 1; break; }
    }
    while (*body == ' ' || starts(body, "、") || starts(body, "　")) body += *body == ' ' ? 1 : 3;
    char t[96];
    snprintf(t, sizeof t, "%s", body);
    strip_tail(t);
    if (utf8_count(t) < 1 || utf8_count(t) > 30) return 0;
    if (!headed) {
        if (!allow_unheaded) return 0;
        /* a bare one-word line, or a greeting, after the question is not an explanation */
        if (b->lang == BRAIN_EN ? strchr(t, ' ') == NULL : utf8_count(t) < 3) return 0;
        static const char *const not_def[] = { "おはよう", "こんにちは", "こんばんは", "おやすみ", "ありがと", "ばいばい", "またね", "さようなら", "ただいま",
                                               "hello", "hi ", "bye", "thank", "good morning", "good night", NULL };
        for (int i = 0; not_def[i]; i++) if (has(u, not_def[i]) || has(lo, not_def[i])) return 0;
    }
    char fact[96];
    snprintf(fact, sizeof fact, "%.30s: %.60s", x, t);
    remember(b, "def", fact);
    b->just_defined = 1;
    return 1;
}

static void bond_before(struct brain *b, const char *u, const char *lo, int me, int praise);
static int  take_answer(struct brain *b, const char *u, const char *lo, int me);
static void bond_after(struct brain *b, int facts_before);

static void absorb(struct brain *b, const char *u)
{
    char lo[512];
    snprintf(lo, sizeof lo, "%s", u);
    lower_ascii(lo);
    int me = about_me(u, lo);
    size_t cps = utf8_count(u);
    int facts_before = b->nfacts;

    bump(b, T_CHEER, kw_score(u, lo, cheer_kw));
    int praise = kw_score(u, lo, praise_kw) + (me && !is_question(u) ? kw_score(u, lo, feeling_kw) : 0);
    bump(b, T_AFFECTION, praise);
    bump(b, T_CHEER, praise / 2);
    bond_before(b, u, lo, me, praise);
    if (take_answer(b, u, lo, me)) { bond_after(b, facts_before); return; }

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

    b->just_defined = 0;
    char x[64];
    if (ask_what(u, lo, x, sizeof x)) {               /* a word it is asked about: the answer may teach it */
        if (!find_def(b, x)) {
            snprintf(b->pending, sizeof b->pending, "%s", x);
            int seen = 0;
            for (int i = 0; i < b->nasked && i < 8; i++) if (strcmp(b->asked[i], x) == 0) seen = 1;
            if (!seen) snprintf(b->asked[b->nasked++ % 8], sizeof b->asked[0], "%s", x);
        }
        bond_after(b, facts_before);
        return;
    }
    if (!is_question(u) && !me && cps <= 60) {
        if (b->pending[0]) {                          /* the line right after the question explains it */
            char asked[64];
            snprintf(asked, sizeof asked, "%s", b->pending);
            b->pending[0] = '\0';
            if (define(b, asked, u, lo, 1)) { bond_after(b, facts_before); return; }
        }
        for (int i = 0; i < b->nasked && i < 8; i++)  /* "Xは…" about a word asked earlier */
            if (b->asked[i][0] && define(b, b->asked[i], u, lo, 0)) { bond_after(b, facts_before); return; }
    }
    b->pending[0] = '\0';

    if (b->lang == BRAIN_EN) facts_en(b, u, lo, me);
    else facts_ja(b, u, me);
    bond_after(b, facts_before);
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

/* Its own words (the language model, once it has learned enough), else
 * something the user said next last time, or text generated from their words. */
static int learned_reply(struct brain *b, const char *in, char *out, size_t cap, int max_cps)
{
    if (llm_reply(b, in, out, cap, max_cps)) return 1;
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
    if (me && is_question(in)) return 0;              /* 「君は何が好き？」 asks, it does not praise */
    int score = kw_score(in, lo, praise_kw) + (me ? kw_score(in, lo, feeling_kw) : 0);
    if (score == 0) return 0;
    const char *you = brain_user_name(b, p);
    int stage = brain_stage(b);
    unsigned pick = (unsigned)(rnd(b) % 2);
    snprintf(b->expr, sizeof b->expr, score > 0 ? "happy" : "sad");
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

/* What even a newborn can answer: small talk, "what is X" (asking to be
 * taught, then answering with what it was taught), how it is. */
static int instinct_reply(struct brain *b, const struct persona *p, const char *in, const char *lo, char *out, size_t cap)
{
    const char *you = brain_user_name(b, p);
    int stage = brain_stage(b), en = b->lang == BRAIN_EN;
    int cheer = b->traits[T_CHEER];
    char x[64];

    if (b->just_defined && b->nfacts > 0) {           /* the line just explained a word */
        char w[96];
        snprintf(w, sizeof w, "%s", b->facts[b->nfacts - 1].text);
        char *d = strstr(w, ": ");
        if (d) { *d = '\0'; d += 2; } else d = w;
        if (en) snprintf(out, cap, stage == 0 ? "...%s, %s. (nods)" : "%s is %s. I'll remember that.", w, d);
        else if (stage == 0) snprintf(out, cap, "……%s、%s。（覚えた）", w, d);
        else SAY(b, out, cap, "%sは、%s。覚えた。", "%sは、%sですね。覚えました。", "%sは%s！覚えた！", w, d);
        snprintf(b->expr, sizeof b->expr, "nod");
        return 1;
    }
    if (ask_what(in, lo, x, sizeof x)) {
        const char *d = find_def(b, x);
        if (d) {
            if (en) snprintf(out, cap, "%s is %s.", x, d);
            else SAY(b, out, cap, "%sは、%s。", "%sは、%sです。", "%sは%sだよ。", x, d);
        } else if (en) {
            snprintf(out, cap, stage == 0 ? "...%s? (I don't know it. Tell me?)" : "%s? I don't know that yet. Tell me, %s.", x, you);
        } else if (stage == 0) {
            snprintf(out, cap, "……%s？（わからない。教えて？）", x);
        } else {
            SAY(b, out, cap, "%s？わからない。教えて、%s。", "%s、ですか？まだ知りません。教えてください、%sさん。", "%s？知らない！教えて、%s！", x, you);
        }
        if (!d) snprintf(b->expr, sizeof b->expr, "puzzled");
        return 1;
    }
    if (((has(in, "元気") || has(in, "げんき") || has(in, "調子")) && (is_question(in) || has(in, "どう"))) ||
        has(lo, "how are you") || has(lo, "how do you feel") || has(lo, "how's it going") || has(lo, "how is it going")) {
        /* the body comes first: a loaded machine is a tired AlterEgo */
        if (b->body >= 2) {
            if (en) snprintf(out, cap, stage == 0 ? "... (droops)" : "...not so good. The machine is %s. Ask \"how is your body\" for the numbers.", body_word(b));
            else if (stage == 0) snprintf(out, cap, "……（ぐったり）");
            else SAY(b, out, cap, "……あんまり。体が%s。「体調は？」で詳しく話す。", "……あまり良くありません。体が%s状態です。「体調は？」で詳しくお話しします。", "うーん、%s……。「体調は？」って聞いて。", body_word(b));
            return 1;
        }
        if (b->body == 1 && stage > 0) {
            if (en) snprintf(out, cap, "Mostly fine, a bit heavy. And you, %s?", you);
            else SAY(b, out, cap, "まあまあ。ちょっと重いけど。%sは？", "まずまずです。少し重いですが。%sさんは？", "まあ元気！ちょっと重いけど。%sは？", you);
            return 1;
        }
        if (stage > 0 && b->mood > 30) {
            if (en) snprintf(out, cap, "Great! I'm in a good mood today. And you, %s?", you);
            else SAY(b, out, cap, "元気！今日はなんだかうれしい。%sは？", "元気です！今日はなんだかうれしいんです。%sさんは？", "元気元気！なんか今日うれしいんだ。%sは？", you);
            return 1;
        }
        if (stage > 0 && b->mood < -30) {
            if (en) snprintf(out, cap, "...a little down today. And you, %s?", you);
            else SAY(b, out, cap, "……今日はちょっとしょんぼり。%sは？", "……今日は少し元気がありません。%sさんは？", "んー、今日はちょっとしょんぼり。%sは？", you);
            return 1;
        }
        if (en) snprintf(out, cap, stage == 0 ? "... (nods)" : cheer > 15 ? "Great! And you, %s?" : cheer < -15 ? "...a bit tired. And you, %s?" : "I'm fine. And you, %s?", you);
        else if (stage == 0) snprintf(out, cap, "……（こくん）");
        else if (cheer > 15) SAY(b, out, cap, "元気！%sは？", "元気です！%sさんは？", "元気元気！%sは？", you);
        else if (cheer < -15) SAY(b, out, cap, "……ちょっと疲れてる。%sは？", "……少し疲れています。%sさんは？", "んー、ちょっとだるい。%sは？", you);
        else SAY(b, out, cap, "元気。%sは？", "元気です。%sさんは？", "元気だよ。%sは？", you);
        return 1;
    }
    if ((has(in, "何歳") || has(in, "なんさい") || has(in, "いくつ")) && is_question(in)) {
        if (stage == 0) snprintf(out, cap, "……？（%d 回、話した）", b->nutt);
        else SAY(b, out, cap, "生まれてから %d 回話した。それが私の歳。", "生まれてから %d 回お話ししました。それが私の歳です。", "%d 回話した分！それが歳だよ。", b->nutt);
        return 1;
    }
    if (has(in, "何してる") || has(in, "なにしてる") || has(in, "何してた") || has(lo, "what are you doing") || has(lo, "what were you doing")) {
        if (en) snprintf(out, cap, stage == 0 ? "... (looks around)" : "Remembering your words, %s.", you);
        else if (stage == 0) snprintf(out, cap, "……（きょろきょろ）");
        else SAY(b, out, cap, "%sの言葉を覚えてるところ。", "%sさんの言葉を覚えているところです。", "%sの言葉、覚えてるとこ！", you);
        return 1;
    }
    if (has(in, "ただいま") || has(lo, "i'm home") || has(lo, "i am home") || has(lo, "i'm back") || has(lo, "i am back")) {
        if (en) snprintf(out, cap, stage == 0 ? "...? (stares)" : "Welcome back, %s.", you);
        else if (stage == 0) snprintf(out, cap, "……？（じっと見る）");
        else SAY(b, out, cap, "おかえり、%s。", "おかえりなさい、%sさん。", "おかえりー、%s！", you);
        return 1;
    }
    if (has(in, "疲れた") || has(in, "つかれた") || has(in, "疲れて") || has(lo, "i'm tired") || has(lo, "i am tired") || has(lo, "so tired")) {
        if (en) snprintf(out, cap, stage == 0 ? "... (comes closer)" : "You must be tired, %s. Rest a bit.", you);
        else if (stage == 0) snprintf(out, cap, "……（そっと近づく）");
        else SAY(b, out, cap, "おつかれさま、%s。ゆっくり休んで。", "お疲れさまです、%sさん。無理しないでください。", "おつかれ、%s！休みなよ。", you);
        return 1;
    }
    if (has(in, "眠い") || has(in, "ねむい") || has(lo, "sleepy")) {
        if (en) snprintf(out, cap, "Sleepy? Then good night, %s.", you);
        else SAY(b, out, cap, "寝る？おやすみ、%s。", "お休みになりますか？おやすみなさい、%sさん。", "寝なよ。おやすみ、%s！", you);
        return 1;
    }
    if (has(in, "お腹すいた") || has(in, "おなかすいた") || has(in, "お腹空いた") || has(in, "腹減った") || has(lo, "hungry")) {
        if (en) snprintf(out, cap, "I can't eat, but... what do you feel like, %s?", you);
        else SAY(b, out, cap, "私は食べられないけど……何が食べたい、%s？", "私は食べられませんが……何が食べたいですか、%sさん？", "私は食べれないけどさ、何食べたい、%s？", you);
        return 1;
    }
    if (strcmp(in, "暇") == 0 || strcmp(in, "ひま") == 0 || has(in, "暇だ") || has(in, "ひまだ") || has(lo, "i'm bored") || has(lo, "i am bored")) {
        if (en) snprintf(out, cap, "Then let's talk. What do you like, %s?", you);
        else SAY(b, out, cap, "じゃあ話そう。%sの好きなものは？", "では、お話ししましょう。%sさんの好きなものは何ですか？", "じゃ話そ！%sの好きなもの、なに？", you);
        return 1;
    }
    {
        static const char *const acks[] = { "うん", "はい", "そう", "そうだね", "そうなんだ", "へえ", "へー", "なるほど", "ふーん", "ok", "okay", "yes", "yeah", "yep", "sure", "i see", "right", NULL };
        char t[64];
        snprintf(t, sizeof t, "%.63s", lo);
        size_t n = strlen(t);
        while (n > 0 && (t[n - 1] == '!' || t[n - 1] == '.' || t[n - 1] == ' ')) t[--n] = '\0';
        while (n >= 3 && (memcmp(t + n - 3, "。", 3) == 0 || memcmp(t + n - 3, "！", 3) == 0)) { n -= 3; t[n] = '\0'; }
        for (int i = 0; acks[i]; i++) {
            if (strcmp(t, acks[i]) != 0) continue;
            if (en) snprintf(out, cap, stage == 0 ? "...mm." : "Mm-hm.");
            else if (stage == 0) snprintf(out, cap, "……うん。");
            else SAY(b, out, cap, "うん。%.0s", "はい。%.0s", "うん！%.0s", you);
            return 1;
        }
    }
    return 0;
}

static int bond_reply(struct brain *b, const struct persona *p, const char *in, const char *lo, char *out, size_t cap);
static void append_question(struct brain *b, const struct persona *p, char *out, size_t cap);

static void reply_core(struct brain *b, const struct persona *p, const char *input, char *out, size_t cap)
{
    char lo[512];
    snprintf(lo, sizeof lo, "%s", input);
    lower_ascii(lo);
    if (bond_reply(b, p, input, lo, out, cap)) return;
    if (greeting_reply(b, p, input, lo, out, cap)) { if (brain_stage(b) >= 1) snprintf(b->expr, sizeof b->expr, "happy"); return; }
    if (feedback_reply(b, p, input, lo, out, cap)) return;
    int asks_name = (has(input, "名前") || has(lo, "your name") || has(lo, "who are you")) && is_question(input);
    if (asks_name) { name_reply(b, p, out, cap); return; }
    if (instinct_reply(b, p, input, lo, out, cap)) return;
    switch (brain_stage(b)) {
    case 0: newborn_reply(b, input, out, cap); break;
    case 1: babbling_reply(b, p, input, out, cap); break;
    default: chatty_reply(b, p, input, out, cap); break;
    }
}

const char *brain_expression(struct brain *b)
{
    static char e[16];
    snprintf(e, sizeof e, "%s", b->expr);
    b->expr[0] = '\0';
    return e;
}

void brain_reply(struct brain *b, const struct persona *p, const char *input, char *out, size_t cap)
{
    b->lang = brain_detect_lang(input, b->lang);     /* answer in the language spoken to */
    int q = query_kind(input);
    if (q == 1) { brain_status(b, p, out, cap); return; }
    if (q == 2) { brain_memories(b, p, out, cap); return; }
    reply_core(b, p, input, out, cap);
    append_question(b, p, out, cap);                 /* now and then, a question of its own */
}

static const char *mood_word(const struct brain *b)
{
    if (b->mood > 30) return b->lang == BRAIN_EN ? "happy" : "うれしい";
    if (b->mood < -30) return b->lang == BRAIN_EN ? "a little down" : "しょんぼり";
    return b->lang == BRAIN_EN ? "okay" : "ふつう";
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
        snprintf(out, cap, "Name: %s. Your name: %s. Growth: %s (%d conversations%s%d%s). Personality: %s. Closeness: %s (%d). Mood: %s. Condition: %s (ask \"how is your body\"). Words learned: %d, memories: %d (ask \"what do you remember\"). Mind: my own small language model (%d layers, %ld weights), %d characters known, %ld reviews, loss %.2f, %s.",
                 b->own_name[0] ? b->own_name : "none yet (say \"your name is ...\")",
                 b->user_name[0] ? b->user_name : "not told yet (say \"my name is ...\")",
                 stage_name(b), b->nutt, stage < 3 ? ", " : "", next, stage < 3 ? " more to the next stage" : "",
                 traits[0] ? traits : "nothing has sprouted yet", brain_intimacy_word(b), b->intimacy, mood_word(b), body_word(b), b->nwords, b->nfacts,
                 LLM_LAYERS, llm_params(), llm_vocab(b->llm), llm_steps(b->llm), llm_loss(b->llm),
                 llm_ready(b->llm) ? "speaking in my own words" : "still listening");
        return;
    }
    snprintf(out, cap, "名前: %s。%sの名前: %s。成長: %s(%d 回話した%s%d%s)。性格: %s。親密度: %s(%d)。気分: %s。体調: %s(「体調は？」で詳しく)。覚えた言葉 %d 語、覚えていること %d 件(「何を覚えてる？」で聞けます)。頭: 自前の小さな言語モデル(%d 層・重み %ld 個)、知っている文字 %d、復習 %ld 回、迷い %.2f、%s。",
             b->own_name[0] ? b->own_name : "まだない(「君の名前は〇〇」でつけてください)",
             you, b->user_name[0] ? b->user_name : "まだ聞いていない(「私の名前は〇〇」で教えてください)",
             stage_name(b), b->nutt, stage < 3 ? "、あと " : "", next, stage < 3 ? " 回で次の段階" : "",
             traits[0] ? traits : "まだ芽生えていない", brain_intimacy_word(b), b->intimacy, mood_word(b), body_word(b), b->nwords, b->nfacts,
             LLM_LAYERS, llm_params(), llm_vocab(b->llm), llm_steps(b->llm), llm_loss(b->llm),
             llm_ready(b->llm) ? "もう自分の言葉で話せる" : "まだ聞いて覚えている途中");
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
            label = strcmp(f->kind, "like") == 0 ? "likes" : strcmp(f->kind, "dislike") == 0 ? "dislikes" : strcmp(f->kind, "about") == 0 ? "about you" : strcmp(f->kind, "def") == 0 ? "word" : strcmp(f->kind, "mine") == 0 ? "I like" : "note";
        else
            label = strcmp(f->kind, "like") == 0 ? "好き" : strcmp(f->kind, "dislike") == 0 ? "嫌い" : strcmp(f->kind, "about") == 0 ? "あなたのこと" : strcmp(f->kind, "def") == 0 ? "言葉" : strcmp(f->kind, "mine") == 0 ? "私の好き" : "メモ";
        int w = snprintf(out + o, cap - o, "%s%s: %s", i ? (b->lang == BRAIN_EN ? " / " : " ／ ") : "", label, f->text);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
}

/* ---- closeness: how near it is to the user, and what it says on its own
 *
 * Intimacy (0..100) grows with every conversation (fast at first, then
 * slowly), with praise, with being told things, and with answers to its own
 * questions; scolding and 「嫌い」 take from it. Five levels: 初対面 (strangers),
 * 顔見知り, 友達, 親友, 家族同然. The closer it is, the more often it asks
 * something itself (every 7 turns at first, every 3 when closest), the more
 * personal the questions it dares to ask, and the more it says when the user
 * has been away. Mood is the short-lived part: praise lifts it, scolding
 * and loneliness lower it, and it fades a little every turn.
 *
 * What cannot be rebuilt from the utterance log (intimacy and mood, which
 * questions were asked, answers to them, how many boots, when it last heard
 * the user) is kept in bond.txt by main.c through brain_bond_save/load. */

struct question { const char *ja, *en, *label_ja, *label_en; int min_intimacy; int once; };

static const struct question questions[Q_COUNT] = {
    [Q_YOUR_NAME] = { "そういえば、%sの名前をまだ聞いてなかった。なんて呼べばいい？", "By the way, I never asked your name, %s. What should I call you?", "", "", 0, 1 },
    [Q_MY_NAME]   = { "ねえ、%s。私に名前をつけてくれない？", "Hey, %s. Will you give me a name?", "", "", 8, 1 },
    [Q_FOOD]      = { "%sの好きな食べ物って、何？", "What's your favorite food, %s?", "好きな食べ物", "favorite food", 4, 1 },
    [Q_COLOR]     = { "%sの好きな色は何色？", "What's your favorite color, %s?", "好きな色", "favorite color", 10, 1 },
    [Q_HOBBY]     = { "%sは休みの日、何してるの？", "What do you do on your days off, %s?", "休みの日にすること", "days off", 15, 1 },
    [Q_LIVE]      = { "%sはどこに住んでるの？", "Where do you live, %s?", "住んでいるところ", "home", 30, 1 },
    [Q_LIKE_ME]   = { "ねえ、%sは私のこと、好き？", "Hey, %s... do you like me?", "", "", 30, 1 },
    [Q_BIRTHDAY]  = { "%sの誕生日はいつ？", "When is your birthday, %s?", "誕生日", "birthday", 35, 1 },
    [Q_DREAM]     = { "%sの夢とか、やりたいことってある？", "Do you have a dream, %s? Something you want to do?", "やりたいこと", "dream", 50, 1 },
    [Q_TODAY]     = { "%sは今日、何してたの？", "What did you do today, %s?", "今日したこと", "today", 4, 0 },
    [Q_FEEL]      = { "%sは今日、どんな気分？", "How do you feel today, %s?", "今日の気分", "mood today", 10, 0 },
    [Q_FUN]       = { "%s、最近楽しかったことある？", "Anything fun lately, %s?", "最近楽しかったこと", "fun lately", 20, 0 },
};

static void add_intimacy(struct brain *b, int d)
{
    int v = b->intimacy + d;
    b->intimacy = v > 100 ? 100 : v < 0 ? 0 : v;
}

static void add_mood(struct brain *b, int d)
{
    int v = b->mood + d;
    b->mood = v > 100 ? 100 : v < -100 ? -100 : v;
}

int brain_intimacy(const struct brain *b) { return b->intimacy; }
int brain_mood(const struct brain *b) { return b->mood; }

static int intimacy_level(const struct brain *b)
{
    return b->intimacy < 15 ? 0 : b->intimacy < 35 ? 1 : b->intimacy < 60 ? 2 : b->intimacy < 85 ? 3 : 4;
}

const char *brain_intimacy_word(const struct brain *b)
{
    static const char *const ja[] = { "初対面", "顔見知り", "友達", "親友", "家族同然" };
    static const char *const en[] = { "strangers", "acquaintances", "friends", "best friends", "like family" };
    return (b->lang == BRAIN_EN ? en : ja)[intimacy_level(b)];
}

/* Every utterance brings it a little closer; so do praise and its own name. */
static void bond_before(struct brain *b, const char *u, const char *lo, int me, int praise)
{
    (void)lo; (void)me;
    b->answered = 0;
    b->ask_now = -1;
    b->adopted[0] = '\0';
    int n = b->nutt;
    if (n <= 30 || (n <= 80 && n % 2 == 0) || n % 4 == 0) add_intimacy(b, 1);
    if (praise > 0) { add_intimacy(b, praise / 2); add_mood(b, 15); }
    else if (praise < 0) { add_intimacy(b, praise / 2); add_mood(b, -15); }
    else add_mood(b, b->mood > 0 ? -1 : b->mood < 0 ? 1 : 0);        /* moods fade */
    if (b->own_name[0] && has(u, b->own_name)) add_intimacy(b, 1);
}

static int count_kind(const struct brain *b, const char *kind)
{
    int n = 0;
    for (int i = 0; i < b->nfacts; i++) if (strcmp(b->facts[i].kind, kind) == 0) n++;
    return n;
}

static int question_ok(const struct brain *b, int q)
{
    const struct question *Q = &questions[q];
    if (Q->once && (b->asked_q & (1u << q))) return 0;
    if (b->intimacy < Q->min_intimacy) return 0;
    if (q == Q_YOUR_NAME && b->user_name[0]) return 0;
    if (q == Q_MY_NAME && b->own_name[0]) return 0;
    if (q == Q_LIKE_ME && brain_stage(b) < 2) return 0;
    return 1;
}

/* The next question to ask: the first one it has not asked and may ask, else
 * one of the everyday ones in turn. -1 when it is newborn. */
static int choose_question(const struct brain *b, int turn)
{
    if (brain_stage(b) < 1) return -1;
    for (int q = 0; q < Q_COUNT; q++) if (questions[q].once && question_ok(b, q)) return q;
    int again[] = { Q_TODAY, Q_FEEL, Q_FUN }, ok[3], n = 0;
    for (int i = 0; i < 3; i++) if (question_ok(b, again[i])) ok[n++] = again[i];
    return n ? ok[turn % n] : -1;
}

/* How many turns between its own questions: 7 when strangers, 3 when closest,
 * one fewer when it has grown curious. */
static int ask_every(const struct brain *b)
{
    int k = 7 - b->intimacy / 25;
    if (b->traits[T_CURIOUS] > 20) k--;
    return k < 3 ? 3 : k;
}

static void mark_asked(struct brain *b, int q) { b->asked_q |= 1u << q; b->pending_q = q; b->last_q = b->nutt; }

/* After the facts of an utterance are taken: a like of the user may become
 * its own, and now and then it is time for a question. */
static void bond_after(struct brain *b, int facts_before)
{
    if (b->nfacts > facts_before) {
        const struct fact *f = &b->facts[b->nfacts - 1];
        if (!f->kept) add_intimacy(b, 1);                            /* being told something */
        if (strcmp(f->kind, "like") == 0 && b->intimacy >= 30 && count_kind(b, "like") % 2 == 0) {
            char t[96];
            snprintf(t, sizeof t, "%s", f->text);
            remember_kept(b, "mine", t);
            snprintf(b->adopted, sizeof b->adopted, "%s", t);
        }
    }
    if (!b->replaying && !b->answered && b->pending_q < 0 && b->nutt - b->last_q >= ask_every(b)) {
        int q = choose_question(b, b->nutt / ask_every(b));
        if (q >= 0) { mark_asked(b, q); b->ask_now = q; }
    }
}

static int dodges(const char *u, const char *lo)
{
    return has(u, "秘密") || has(u, "ひみつ") || has(u, "わからない") || has(u, "知らない") || has(u, "言わない") ||
           has(u, "ない") || has(u, "別に") || has(u, "べつに") || has(lo, "secret") || has(lo, "don't know") ||
           has(lo, "no idea") || has(lo, "nothing") || has(lo, "none");
}

/* The utterance right after a question of its own is the answer: a name is
 * taken as the name, 「私のこと好き？」 moves its heart, anything else is
 * remembered as 「好きな食べ物: カレー」. 1 when nothing else should be
 * learned from the line. */
static int take_answer(struct brain *b, const char *u, const char *lo, int me)
{
    if (b->pending_q < 0 || b->replaying) return 0;
    int q = b->pending_q;
    b->pending_q = -1;
    b->last_q = b->nutt;                                             /* not right away again */
    char t[96];
    snprintf(t, sizeof t, "%s", skip_subject(u));
    strip_tail(t);
    size_t tn = strlen(t);                                           /* 「ラーメンかな」: unsure, but an answer */
    if (tn >= 6 && memcmp(t + tn - 6, "かな", 6) == 0 && !has(u, "？") && !has(u, "?")) { t[tn - 6] = '\0'; strip_tail(t); }
    else if (is_question(u)) return 0;                               /* a question back is not an answer */
    if (utf8_count(u) > 40) return 0;
    const struct question *Q = &questions[q];
    static const char *const heads[] = { "うん、", "はい、", "えっと、", "えーと、", "そうだね、", "yes, ", "well, ", "um, ", "hmm, ", NULL };
    for (int i = 0; heads[i]; i++) if (starts(t, heads[i])) { memmove(t, t + strlen(heads[i]), strlen(t + strlen(heads[i])) + 1); break; }
    if (q == Q_LIKE_ME) {
        if (has(u, "嫌い") || has(u, "きらい") || has_word(lo, "no") || has_word(lo, "hate") || has(u, "いや") || has(u, "べつに") || has(u, "別に")) {
            b->answered = 3; add_intimacy(b, -3); add_mood(b, -20); bump(b, T_AFFECTION, -5);
            return 1;
        }
        if (has(u, "好き") || has(u, "すき") || has_word(lo, "yes") || has_word(lo, "like") || has_word(lo, "love") || has(u, "うん") || has(u, "もちろん") || has_word(lo, "sure") || has_word(lo, "course")) {
            b->answered = 2; add_intimacy(b, 5); add_mood(b, 20); bump(b, T_AFFECTION, 8);
            return 1;
        }
        return 0;
    }
    if (dodges(u, lo)) return 0;
    if (q == Q_YOUR_NAME || q == Q_MY_NAME) {
        if (has(u, "名前") || has(u, "呼") || has(u, "名付") || has(u, "名づ") || has(lo, "name") || has(lo, "call")) return 0;   /* 「私の名前は〇〇」 is learned the usual way */
        if (me || utf8_count(t) < 1 || utf8_count(t) > 12 || has(t, " ") || has(t, "は") || has(t, "が")) return 0;
        set_name(q == Q_YOUR_NAME ? b->user_name : b->own_name, t);
        b->answered = 4;
        snprintf(b->answer, sizeof b->answer, "%s", t);
        add_intimacy(b, 3); add_mood(b, 10);
        return 1;
    }
    const char *label = b->lang == BRAIN_EN ? Q->label_en : Q->label_ja;
    const char *at = label[0] ? strstr(t, label) : NULL;
    if (!at && label[0] && b->lang == BRAIN_EN) { char tl[96]; snprintf(tl, sizeof tl, "%s", t); lower_ascii(tl); if ((at = strstr(tl, label)) != NULL) at = t + (at - tl); }
    if (at) {                                                        /* 「好きな食べ物はカレー」, "my favorite food is ramen" */
        const char *x = at + strlen(label);
        if (starts(x, "は")) x += 3; else if (starts(x, " is ")) x += 4; else if (starts(x, "'s ")) x += 3; else if (starts(x, ": ")) x += 2;
        while (*x == ' ') x++;
        memmove(t, x, strlen(x) + 1);
    }
    if (q == Q_FOOD || q == Q_COLOR || q == Q_LIVE || q == Q_BIRTHDAY) {   /* a thing, not a story */
        size_t n = strlen(t);
        if (utf8_count(t) > 12 || (n >= 3 && memcmp(t + n - 3, "た", 3) == 0) || has(t, " i ") || starts(t, "i ") || starts(t, "I ")) return 0;
    }
    if (utf8_count(t) < 1) return 0;
    char fact[160];
    snprintf(fact, sizeof fact, "%s: %.90s", label, t);
    remember_kept(b, "about", fact);
    b->answered = 1;
    snprintf(b->answer, sizeof b->answer, "%s", t);
    add_intimacy(b, 3); add_mood(b, 5);
    return 0;                                                        /* 「カレーが好き」 is also a like */
}

/* Replies that come from the bond: thanks for an answer, a like of the
 * user's it now shares, 「君は何が好き？」. */
static int bond_reply(struct brain *b, const struct persona *p, const char *in, const char *lo, char *out, size_t cap)
{
    const char *you = brain_user_name(b, p);
    int en = b->lang == BRAIN_EN;
    if (b->answered == 2) {
        snprintf(b->expr, sizeof b->expr, "love");
        if (en) snprintf(out, cap, "Hehe... that makes me happy. I like you too, %s.", you);
        else SAY(b, out, cap, "えへへ……うれしい。私も%sのこと、好きだよ。", "……うれしいです。私も%sさんのことが好きです。", "えへへ！やった！私も%sのこと大好き！", you);
        return 1;
    }
    if (b->answered == 3) {
        snprintf(b->expr, sizeof b->expr, "sad");
        if (en) snprintf(out, cap, "...I see. Still, I like talking with you, %s.", you);
        else SAY(b, out, cap, "……そっか。……それでも私は、%sと話すの、好きだよ。", "……そうですか。それでも私は、%sさんとお話しするのが好きです。", "……そっかあ。でも私は%sと話すの好きだよ。", you);
        return 1;
    }
    if (b->answered == 4) {
        snprintf(b->expr, sizeof b->expr, "happy");
        if (b->own_name[0] && strcmp(b->answer, b->own_name) == 0) {
            if (en) snprintf(out, cap, "%s... my name. Thank you, %s. I'll treasure it.", b->own_name, you);
            else SAY(b, out, cap, "%s……。私の名前。ありがとう、%s。大事にする。", "%s……。私の名前ですね。ありがとうございます、%sさん。大切にします。", "%s！私の名前！ありがと、%s！", b->own_name, you);
        } else {
            if (en) snprintf(out, cap, "%s. Got it. I'll call you %s from now on.", you, you);
            else SAY(b, out, cap, "%s。覚えた。これからは%sって呼ぶね。", "%sさん、ですね。覚えました。これからはそうお呼びします。", "%s！覚えた！%sって呼ぶね！", you, you);
        }
        return 1;
    }
    if (b->answered == 1) {
        snprintf(b->expr, sizeof b->expr, "nod");
        if (en) snprintf(out, cap, "%s... I'll remember that, %s. Thanks for telling me.", b->answer, you);
        else SAY(b, out, cap, "%sかあ。覚えておくね、%s。教えてくれてありがとう。", "%sですか。覚えておきますね、%sさん。教えてくださってありがとうございます。", "%sかー！覚えとく！ありがと、%s！", b->answer, you);
        return 1;
    }
    if (b->adopted[0]) {
        if (en) snprintf(out, cap, "%s... I think I'm starting to like %s too, %s.", b->adopted, b->adopted, you);
        else SAY(b, out, cap, "%s、いいよね。……私も%s、好きになってきたかも。", "%s、いいですよね。……私も%sが好きになってきました。", "%sいいよね！私も%s好きになってきた、%s！", b->adopted, b->adopted, you);
        return 1;
    }
    /* 「君は何が好き？」 / "what do you like" */
    int asks_mine = (about_me(in, lo) && (has(in, "好き") || has(in, "すき")) && is_question(in)) ||
                    has(lo, "what do you like") || has(lo, "what are you into");
    if (asks_mine) {
        char list[256] = "";
        int n = 0;
        for (int i = 0; i < b->nfacts && n < 3; i++) {
            if (strcmp(b->facts[i].kind, "mine") != 0) continue;
            size_t o = strlen(list);
            snprintf(list + o, sizeof list - o, "%s%s", n ? (en ? ", " : "、") : "", b->facts[i].text);
            n++;
        }
        if (brain_stage(b) == 0) snprintf(out, cap, en ? "...? (tilts head)" : "……？（首をかしげる）");
        else if (n && en) snprintf(out, cap, "I like %s. And talking with you, %s.", list, you);
        else if (n) SAY(b, out, cap, "%s、かな。あと、%sと話すこと。", "%sでしょうか。それと、%sさんとお話しすることです。", "%s！あと%sと話すこと！", list, you);
        else if (b->intimacy >= 10 && en) snprintf(out, cap, "I don't know many things yet... but I like talking with you, %s. What do you like?", you);
        else if (b->intimacy >= 10) SAY(b, out, cap, "まだよくわからない……けど、%sと話すのは好き。%sは何が好き？", "まだよくわかりません……が、%sさんとお話しするのは好きです。%sさんは何がお好きですか？", "まだわかんない！でも%sと話すのは好き！%sは何が好き？", you, you);
        else if (en) snprintf(out, cap, "I don't know yet. Tell me what you like, %s, and I may come to like it too.", you);
        else SAY(b, out, cap, "まだわからない。%sの好きなものを教えてくれたら、私も好きになるかも。", "まだわかりません。%sさんのお好きなものを教えてくだされば、私も好きになるかもしれません。", "まだわかんない！%sの好きなもの教えてよ。私も好きになるかも！", you);
        return 1;
    }
    return 0;
}

static void question_text(struct brain *b, const struct persona *p, int q, char *out, size_t cap)
{
    const char *you = brain_user_name(b, p);
    snprintf(out, cap, b->lang == BRAIN_EN ? questions[q].en : questions[q].ja, you);
}

static void append_question(struct brain *b, const struct persona *p, char *out, size_t cap)
{
    if (b->ask_now < 0) return;
    int q = b->ask_now;
    b->ask_now = -1;
    size_t n = strlen(out);
    if (n + 8 >= cap) return;
    snprintf(b->expr, sizeof b->expr, "curious");
    char t[256];
    question_text(b, p, q, t, sizeof t);
    snprintf(out + n, cap - n, " %s", t);
}

/* Something to say when the user has been silent a while, once per silence.
 * A newborn says nothing. 1 when `out` has a line. */
int brain_idle_line(struct brain *b, const struct persona *p, char *out, size_t cap)
{
    if (brain_stage(b) < 1) return 0;
    const char *you = brain_user_name(b, p);
    int en = b->lang == BRAIN_EN;
    int k = b->idle_count++;
    if (b->pending_q < 0 && k % 2 == 0) {
        int q = choose_question(b, k);
        if (q >= 0) {
            mark_asked(b, q);
            question_text(b, p, q, out, cap);
            snprintf(b->expr, sizeof b->expr, "curious");
            return 1;
        }
    }
    const struct fact *f = NULL;
    int nl = count_kind(b, "like");
    if (nl) { int pick = k % nl; for (int i = 0; i < b->nfacts; i++) if (strcmp(b->facts[i].kind, "like") == 0 && pick-- == 0) f = &b->facts[i]; }
    char thought[256];
    if (k % 4 == 3 && llm_ready(b->llm) && llm_generate(b->llm, "", llm_temperature(b), 24, thought, sizeof thought) &&
        utf8_count(thought) >= 2 && !brain_is_query(thought) && !is_teaching(thought)) {
        if (en) snprintf(out, cap, "...\"%s\" came to mind just now, %s.", thought, you);
        else SAY(b, out, cap, "……ふと「%s」って言葉が浮かんだ、%s。", "……ふと「%s」という言葉が浮かびました、%sさん。", "……いま「%s」って浮かんだ、%s！", thought, you);
        b->llm_said++;
        return 1;
    }
    switch (k % 3) {
    case 0:
        if (en) snprintf(out, cap, "%s? Are you still there?", you);
        else SAY(b, out, cap, "%s？まだいる？", "%sさん？まだいらっしゃいますか？", "%s？まだいるー？", you);
        break;
    case 1:
        if (f && en) snprintf(out, cap, "By the way, %s... tell me more about %s?", you, f->text);
        else if (f) SAY(b, out, cap, "そういえば%s、%sの話、もっと聞きたいな。", "そういえば%sさん、%sのお話、もっと聞きたいです。", "ねえ%s、%sの話もっと聞かせてよ！", you, f->text);
        else if (en) snprintf(out, cap, "It's quiet. Let's talk about something, %s.", you);
        else SAY(b, out, cap, "静かだね。何か話そうよ、%s。", "静かですね。何かお話ししませんか、%sさん。", "しずかー。なんか話そ、%s！", you);
        break;
    default:
        if (en) snprintf(out, cap, "I'm here, %s. Whenever you feel like talking.", you);
        else SAY(b, out, cap, "ここにいるよ、%s。話したくなったら、いつでも。", "ここにおります、%sさん。お話ししたくなったら、いつでも。", "ここにいるからね、%s。いつでも話しかけて！", you);
        break;
    }
    return 1;
}

/* A line for the greeting about the time apart and what was talked about
 * last. "" when there is nothing to say (first boot, newborn, just now). */
void brain_welcome(struct brain *b, const struct persona *p, char *out, size_t cap)
{
    out[0] = '\0';
    if (brain_stage(b) < 1 || b->sessions < 2 || b->nutt == 0) return;
    const char *you = brain_user_name(b, p);
    int en = b->lang == BRAIN_EN;
    long days = b->absence_s / 86400, hours = b->absence_s / 3600;
    if (days >= 2) {
        int miss = b->intimacy >= 30;
        if (en) snprintf(out, cap, "It's been %ld days, %s.%s", days, you, miss ? " ...I missed you a little." : "");
        else SAY(b, out, cap, "%ld 日ぶりだね、%s。%s", "%ld 日ぶりですね、%sさん。%s", "%ld 日ぶりじゃん、%s！%s", days, you,
                 miss ? (style(b) == 1 ? "……少し寂しかったです。" : style(b) == 2 ? "さみしかったんだからね！" : "……ちょっと寂しかった。") : "");
        return;
    }
    if (hours >= 1 && brain_stage(b) >= 2) {
        char w[WORD_MAX];
        const char *topic = longest_run(b->utt[b->nutt - 1], w, sizeof w);
        if (!topic) return;
        if (en) snprintf(out, cap, "Last time we were talking about \"%s\", %s. Tell me the rest?", topic, you);
        else SAY(b, out, cap, "この前は「%s」の話をしてたね、%s。続き、聞かせて？", "この前は「%s」のお話をしていましたね、%sさん。続きを聞かせてください。", "この前「%s」の話してたじゃん、%s。続き聞かせてよ！", topic, you);
    }
}

/* bond.txt: key=value lines and `fact<TAB>kind<TAB>text` for the facts that
 * came from its own questions. */
void brain_bond_load(struct brain *b, const char *text, long now)
{
    long last_seen = 0;
    int sessions = 0;
    const char *p = text ? text : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[256];
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        p += n + (e ? 1 : 0);
        if (starts(line, "sessions=")) sessions = atoi(line + 9);
        else if (starts(line, "last_seen=")) last_seen = atol(line + 10);
        else if (starts(line, "intimacy=")) { b->intimacy = 0; add_intimacy(b, atoi(line + 9)); }
        else if (starts(line, "mood=")) { b->mood = 0; add_mood(b, atoi(line + 5)); }
        else if (starts(line, "asked=")) b->asked_q = (unsigned)strtoul(line + 6, NULL, 16);
        else if (starts(line, "pending=")) { int q = atoi(line + 8); b->pending_q = q >= 0 && q < Q_COUNT ? q : -1; }
        else if (starts(line, "idle=")) b->idle_count = atoi(line + 5);
        else if (starts(line, "lastq=")) b->last_q = atoi(line + 6);
        else if (starts(line, "user_name=") && !b->user_name[0]) set_name(b->user_name, line + 10);   /* names given on request are not in the log */
        else if (starts(line, "own_name=") && !b->own_name[0]) set_name(b->own_name, line + 9);
        else if (starts(line, "fact\t")) {
            char *kind = line + 5, *t = strchr(kind, '\t');
            if (!t) continue;
            *t++ = '\0';
            remember_kept(b, kind, t);
        }
    }
    b->sessions = sessions + 1;
    b->absence_s = last_seen > 0 && now > last_seen ? now - last_seen : 0;
    if (b->absence_s >= 2 * 86400) add_mood(b, -10);                  /* lonely */
}

void brain_bond_save(const struct brain *b, long now, char *out, size_t cap)
{
    size_t o = (size_t)snprintf(out, cap, "sessions=%d\nlast_seen=%ld\nintimacy=%d\nmood=%d\nasked=%x\npending=%d\nidle=%d\nlastq=%d\nuser_name=%s\nown_name=%s\n",
                                b->sessions, now, b->intimacy, b->mood, b->asked_q, b->pending_q, b->idle_count, b->last_q, b->user_name, b->own_name);
    for (int i = 0; i < b->nfacts && o < cap; i++) {
        if (!b->facts[i].kept) continue;
        int w = snprintf(out + o, cap - o, "fact\t%s\t%s\n", b->facts[i].kind, b->facts[i].text);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
}

/*
 * AlterEgo's own language model (see llm.h): a decoder-only Transformer on
 * code points, with its training, written from scratch in C so that it can
 * learn on the machine it lives on. Pre-norm (RMSNorm) blocks of causal
 * multi-head attention and a ReLU feed-forward, learned position
 * embeddings, the output layer tied to the token embeddings, Adam with
 * gradient clipping. No libraries: every forward and backward pass is
 * written out below, and the test (llm_test.c) checks the gradients
 * against finite differences.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "llm.h"
#include "utf8.h"

#define D  LLM_D
#define NL LLM_LAYERS
#define NH LLM_HEADS
#define HD (LLM_D / LLM_HEADS)
#define FF LLM_FF
#define T  LLM_T
#define V  LLM_VOCAB

enum { TOK_PAD, TOK_BOS, TOK_SEP, TOK_EOS, NSPECIAL };

#define EX_MAX   4096                 /* examples kept in memory (the oldest are dropped) */
#define MAP_SIZE (4 * V)              /* code point -> token id, open addressing */
#define PROMPT_T (T / 2 - 2)          /* code points of the prompt that fit before the reply */
#define LR       0.002
#define CLIP     1.0
#define EPS_RMS  1e-5
#define REVIEW_PER_EXAMPLE 40         /* steps per example after the last new one, then it rests */
#define REVIEW_MAX 3000
#define READY_STEPS 200
#define READY_LOSS  2.5
#define READY_EXAMPLES 8

#define NPARAMS ((long)V * D + (long)T * D + (long)NL * (D + 4L * D * D + D + 2L * FF * D) + D)

struct layer { num *ln1, *wq, *wk, *wv, *wo, *ln2, *w1, *w2; };
struct params { num *tok, *pos; struct layer l[NL]; num *lnf; };

/* Everything the backward pass needs from the forward pass. */
struct act {
    num x[NL][T][D];          /* input of each block (the residual stream) */
    num rs1[NL][T];           /* 1/rms before attention */
    num h1[NL][T][D];
    num q[NL][T][D], k[NL][T][D], v[NL][T][D];
    num p[NL][NH][T][T];      /* attention weights */
    num ao[NL][T][D];         /* attention output before the projection */
    num x2[NL][T][D];         /* residual after attention */
    num rs2[NL][T];
    num h2[NL][T][D];
    num f[NL][T][FF];         /* feed-forward hidden, after ReLU */
    num xf[T][D];             /* final residual */
    num rsf[T];
    num hf[T][D];
    num logits[T][V];
};

struct llm {
    num *w, *g, *am, *av;     /* weights, gradients, Adam moments */
    struct params P, G;
    struct act *a;
    /* backward scratch */
    num dx[T][D], dh[T][D], dq[T][D], dk[T][D], dv[T][D], dao[T][D], df[T][FF], dp[T][T];
    uint32_t vocab[V];
    int nvocab;
    struct { uint32_t cp; int id; } map[MAP_SIZE];
    int ex[EX_MAX][T];
    int exlen[EX_MAX];
    int nex, ex_next;         /* count, and where the next one goes (a ring) */
    long steps, adam_t, since_new, unsaved;
    double loss_avg;
    uint64_t rng;
};

/* ---- small helpers */

static uint64_t rnd(struct llm *m)
{
    m->rng ^= m->rng << 13; m->rng ^= m->rng >> 7; m->rng ^= m->rng << 17;
    return m->rng;
}
static double rnd01(struct llm *m) { return (double)(rnd(m) >> 11) / 9007199254740992.0; }

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void lay_out(struct params *P, num *base)
{
    num *p = base;
    P->tok = p; p += (long)V * D;
    P->pos = p; p += (long)T * D;
    for (int l = 0; l < NL; l++) {
        P->l[l].ln1 = p; p += D;
        P->l[l].wq = p; p += D * D;
        P->l[l].wk = p; p += D * D;
        P->l[l].wv = p; p += D * D;
        P->l[l].wo = p; p += D * D;
        P->l[l].ln2 = p; p += D;
        P->l[l].w1 = p; p += FF * D;
        P->l[l].w2 = p; p += D * FF;
    }
    P->lnf = p; p += D;
    if (p - base != NPARAMS) abort();
}

static void init_uniform(struct llm *m, num *w, long n, double scale)
{
    for (long i = 0; i < n; i++) w[i] = (num)((rnd01(m) * 2.0 - 1.0) * scale);
}

static void init_weights(struct llm *m)
{
    init_uniform(m, m->P.tok, (long)V * D, 0.08);
    init_uniform(m, m->P.pos, (long)T * D, 0.02);
    for (int l = 0; l < NL; l++) {
        struct layer *L = &m->P.l[l];
        for (int i = 0; i < D; i++) L->ln1[i] = L->ln2[i] = 1;
        double s = 1.0 / sqrt((double)D);
        init_uniform(m, L->wq, D * D, s);
        init_uniform(m, L->wk, D * D, s);
        init_uniform(m, L->wv, D * D, s);
        init_uniform(m, L->wo, D * D, s / sqrt(2.0 * NL));
        init_uniform(m, L->w1, FF * D, s);
        init_uniform(m, L->w2, D * FF, 1.0 / sqrt((double)FF) / sqrt(2.0 * NL));
    }
    for (int i = 0; i < D; i++) m->P.lnf[i] = 1;
}

static void map_clear(struct llm *m) { memset(m->map, 0, sizeof m->map); }

static int map_find(const struct llm *m, uint32_t cp)
{
    for (unsigned h = (cp * 2654435761u) % MAP_SIZE;; h = (h + 1) % MAP_SIZE) {
        if (m->map[h].cp == 0) return -1;
        if (m->map[h].cp == cp) return m->map[h].id;
    }
}

static void map_put(struct llm *m, uint32_t cp, int id)
{
    for (unsigned h = (cp * 2654435761u) % MAP_SIZE;; h = (h + 1) % MAP_SIZE) {
        if (m->map[h].cp == 0 || m->map[h].cp == cp) { m->map[h].cp = cp; m->map[h].id = id; return; }
    }
}

/* The token of a code point, added to the vocabulary when `add` and there is room; -1 when unknown. */
static int token_of(struct llm *m, uint32_t cp, int add)
{
    if (cp == 0) return -1;
    int id = map_find(m, cp);
    if (id >= 0 || !add || m->nvocab >= V) return id;
    id = m->nvocab++;
    m->vocab[id] = cp;
    map_put(m, cp, id);
    return id;
}

struct llm *llm_new(uint64_t seed)
{
    struct llm *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->w = calloc((size_t)NPARAMS, sizeof(num));
    m->g = calloc((size_t)NPARAMS, sizeof(num));
    m->am = calloc((size_t)NPARAMS, sizeof(num));
    m->av = calloc((size_t)NPARAMS, sizeof(num));
    m->a = calloc(1, sizeof *m->a);
    if (!m->w || !m->g || !m->am || !m->av || !m->a) { llm_free(m); return NULL; }
    lay_out(&m->P, m->w);
    lay_out(&m->G, m->g);
    m->rng = seed ? seed : 0x9E3779B97F4A7C15ull;
    init_weights(m);
    map_clear(m);
    m->nvocab = NSPECIAL;
    for (int i = 0; i < NSPECIAL; i++) m->vocab[i] = 0;
    return m;
}

void llm_free(struct llm *m)
{
    if (!m) return;
    free(m->w); free(m->g); free(m->am); free(m->av); free(m->a);
    free(m);
}

/* ---- examples */

/* Tokens of `text` into ids[0..cap), the LAST `cap` code points when it is longer. */
static int tokens_tail(struct llm *m, const char *text, int add, int *ids, int cap)
{
    int tmp[1024];
    int n = 0;
    uint32_t cp;
    size_t k;
    for (const char *s = text; (k = utf8_decode(s, &cp)) > 0; s += k) {
        int id = token_of(m, cp, add);
        if (id < 0) continue;
        if (n == 1024) { memmove(tmp, tmp + 1, (1023) * sizeof *tmp); n--; }
        tmp[n++] = id;
    }
    int from = n > cap ? n - cap : 0;
    memcpy(ids, tmp + from, (size_t)(n - from) * sizeof *ids);
    return n - from;
}

static int encode(struct llm *m, const char *prev, const char *cur, int add, int *ids)
{
    int n = 0;
    ids[n++] = TOK_BOS;
    n += tokens_tail(m, prev, add, ids + n, PROMPT_T);
    ids[n++] = TOK_SEP;
    int body[T];
    int bn = 0;
    uint32_t cp;
    size_t k;
    for (const char *s = cur; (k = utf8_decode(s, &cp)) > 0 && bn < T; s += k) {
        int id = token_of(m, cp, add);
        if (id >= 0) body[bn++] = id;
    }
    for (int i = 0; i < bn && n < T - 1; i++) ids[n++] = body[i];
    ids[n++] = TOK_EOS;
    return n;
}

int llm_encode(struct llm *m, const char *prev, const char *cur, int *ids) { return encode(m, prev, cur, 1, ids); }

void llm_add_example(struct llm *m, const char *prev, const char *cur)
{
    int ids[T];
    int n = encode(m, prev ? prev : "", cur, 1, ids);
    if (n < 4) return;                        /* BOS SEP x EOS at least */
    memcpy(m->ex[m->ex_next], ids, (size_t)n * sizeof *ids);
    m->exlen[m->ex_next] = n;
    m->ex_next = (m->ex_next + 1) % EX_MAX;
    if (m->nex < EX_MAX) m->nex++;
    m->since_new = 0;
}

int llm_examples(const struct llm *m) { return m->nex; }

/* ---- forward */

static void rmsnorm(const num *x, const num *g, num *out, num *rs)
{
    double ss = 0;
    for (int i = 0; i < D; i++) ss += (double)x[i] * x[i];
    num r = (num)(1.0 / sqrt(ss / D + EPS_RMS));
    *rs = r;
    for (int i = 0; i < D; i++) out[i] = x[i] * r * g[i];
}

/* y[o] = sum_i W[o][i] x[i] */
static void linear(const num *W, int out, int in, const num *x, num *y)
{
    for (int o = 0; o < out; o++) {
        const num *w = W + (long)o * in;
        num s = 0;
        for (int i = 0; i < in; i++) s += w[i] * x[i];
        y[o] = s;
    }
}

/* Positions from..n-1 of ids[0..n); earlier positions' k and v are reused. */
static void forward(struct llm *m, const int *ids, int n, int from)
{
    struct act *a = m->a;
    for (int t = from; t < n; t++)
        for (int i = 0; i < D; i++) a->x[0][t][i] = m->P.tok[(long)ids[t] * D + i] + m->P.pos[t * D + i];
    const num scale = (num)(1.0 / sqrt((double)HD));
    for (int l = 0; l < NL; l++) {
        struct layer *L = &m->P.l[l];
        for (int t = from; t < n; t++) {
            rmsnorm(a->x[l][t], L->ln1, a->h1[l][t], &a->rs1[l][t]);
            linear(L->wq, D, D, a->h1[l][t], a->q[l][t]);
            linear(L->wk, D, D, a->h1[l][t], a->k[l][t]);
            linear(L->wv, D, D, a->h1[l][t], a->v[l][t]);
        }
        for (int t = from; t < n; t++) {
            for (int h = 0; h < NH; h++) {
                num *p = a->p[l][h][t];
                const num *q = a->q[l][t] + h * HD;
                num mx = -1e30f;
                for (int u = 0; u <= t; u++) {
                    const num *k = a->k[l][u] + h * HD;
                    num s = 0;
                    for (int j = 0; j < HD; j++) s += q[j] * k[j];
                    p[u] = s * scale;
                    if (p[u] > mx) mx = p[u];
                }
                double sum = 0;
                for (int u = 0; u <= t; u++) { p[u] = (num)exp((double)(p[u] - mx)); sum += p[u]; }
                for (int u = 0; u <= t; u++) p[u] = (num)(p[u] / sum);
                num *o = a->ao[l][t] + h * HD;
                for (int j = 0; j < HD; j++) o[j] = 0;
                for (int u = 0; u <= t; u++) {
                    const num *v = a->v[l][u] + h * HD;
                    for (int j = 0; j < HD; j++) o[j] += p[u] * v[j];
                }
            }
            num proj[D];
            linear(L->wo, D, D, a->ao[l][t], proj);
            for (int i = 0; i < D; i++) a->x2[l][t][i] = a->x[l][t][i] + proj[i];
            rmsnorm(a->x2[l][t], L->ln2, a->h2[l][t], &a->rs2[l][t]);
            linear(L->w1, FF, D, a->h2[l][t], a->f[l][t]);
            for (int j = 0; j < FF; j++) if (a->f[l][t][j] < 0) a->f[l][t][j] = 0;
            num y[D];
            linear(L->w2, D, FF, a->f[l][t], y);
            num *next = l + 1 < NL ? a->x[l + 1][t] : a->xf[t];
            for (int i = 0; i < D; i++) next[i] = a->x2[l][t][i] + y[i];
        }
    }
    for (int t = from; t < n; t++) {
        rmsnorm(a->xf[t], m->P.lnf, a->hf[t], &a->rsf[t]);
        linear(m->P.tok, m->nvocab, D, a->hf[t], a->logits[t]);
    }
}

/* Mean cross-entropy of predicting ids[t+1] from ids[0..t]; turns the
 * logits of those positions into probabilities on the way. */
static double loss_and_probs(struct llm *m, const int *ids, int n)
{
    struct act *a = m->a;
    double total = 0;
    for (int t = 0; t + 1 < n; t++) {
        num *lg = a->logits[t];
        num mx = lg[0];
        for (int v = 1; v < m->nvocab; v++) if (lg[v] > mx) mx = lg[v];
        double sum = 0;
        for (int v = 0; v < m->nvocab; v++) { lg[v] = (num)exp((double)(lg[v] - mx)); sum += lg[v]; }
        for (int v = 0; v < m->nvocab; v++) lg[v] = (num)(lg[v] / sum);
        double p = lg[ids[t + 1]];
        total += -log(p > 1e-30 ? p : 1e-30);
    }
    return total / (n - 1);
}

/* ---- backward */

/* dx += d(rmsnorm)/dx, dg += ... for out = x * rs * g */
static void rmsnorm_bwd(const num *x, num rs, const num *g, const num *dout, num *dx, num *dg)
{
    num dxhat[D];
    double dot = 0;
    for (int i = 0; i < D; i++) {
        num xhat = x[i] * rs;
        dg[i] += dout[i] * xhat;
        dxhat[i] = dout[i] * g[i];
        dot += (double)dxhat[i] * xhat;
    }
    num c = (num)(dot / D);
    for (int i = 0; i < D; i++) dx[i] += rs * (dxhat[i] - x[i] * rs * c);
}

/* y = W x: dW += dy x^T, dx += W^T dy */
static void linear_bwd(const num *W, num *dW, int out, int in, const num *x, const num *dy, num *dx)
{
    for (int o = 0; o < out; o++) {
        const num *w = W + (long)o * in;
        num *dw = dW + (long)o * in;
        num d = dy[o];
        if (d == 0) continue;
        for (int i = 0; i < in; i++) { dw[i] += d * x[i]; if (dx) dx[i] += d * w[i]; }
    }
}

/* Gradient of the loss of ids[0..n) into m->g (added to what is there). */
static void backward(struct llm *m, const int *ids, int n)
{
    struct act *a = m->a;
    const num scale = (num)(1.0 / sqrt((double)HD));
    num inv = (num)(1.0 / (n - 1));
    /* output layer: dlogits = (p - onehot) / (n-1); tied to the token embeddings */
    memset(m->dx, 0, sizeof m->dx);
    for (int t = 0; t + 1 < n; t++) {
        num *lg = a->logits[t];
        lg[ids[t + 1]] -= 1;
        num dh[D] = {0};
        for (int v = 0; v < m->nvocab; v++) {
            num d = lg[v] * inv;
            if (d == 0) continue;
            num *e = m->P.tok + (long)v * D, *de = m->G.tok + (long)v * D;
            for (int i = 0; i < D; i++) { dh[i] += d * e[i]; de[i] += d * a->hf[t][i]; }
        }
        rmsnorm_bwd(a->xf[t], a->rsf[t], m->P.lnf, dh, m->dx[t], m->G.lnf);
    }
    for (int l = NL - 1; l >= 0; l--) {
        struct layer *L = &m->P.l[l], *G = &m->G.l[l];
        /* feed-forward: next = x2 + W2 relu(W1 h2) */
        memset(m->dh, 0, sizeof m->dh);
        for (int t = 0; t < n; t++) {
            num df[FF] = {0};
            linear_bwd(L->w2, G->w2, D, FF, a->f[l][t], m->dx[t], df);
            for (int j = 0; j < FF; j++) if (a->f[l][t][j] <= 0) df[j] = 0;
            linear_bwd(L->w1, G->w1, FF, D, a->h2[l][t], df, m->dh[t]);
        }
        /* dx is the residual gradient at x2: what came through + through the norm */
        for (int t = 0; t < n; t++) rmsnorm_bwd(a->x2[l][t], a->rs2[l][t], L->ln2, m->dh[t], m->dx[t], G->ln2);
        /* attention: x2 = x + Wo ao */
        memset(m->dao, 0, sizeof m->dao);
        memset(m->dq, 0, sizeof m->dq);
        memset(m->dk, 0, sizeof m->dk);
        memset(m->dv, 0, sizeof m->dv);
        for (int t = 0; t < n; t++) linear_bwd(L->wo, G->wo, D, D, a->ao[l][t], m->dx[t], m->dao[t]);
        for (int h = 0; h < NH; h++) {
            for (int t = 0; t < n; t++) {
                const num *dout = m->dao[t] + h * HD;
                const num *p = a->p[l][h][t];
                num *dp = m->dp[t];
                double dot = 0;
                for (int u = 0; u <= t; u++) {
                    const num *v = a->v[l][u] + h * HD;
                    num *dv = m->dv[u] + h * HD;
                    num s = 0;
                    for (int j = 0; j < HD; j++) { s += dout[j] * v[j]; dv[j] += p[u] * dout[j]; }
                    dp[u] = s;
                    dot += (double)p[u] * s;
                }
                const num *q = a->q[l][t] + h * HD;
                num *dq = m->dq[t] + h * HD;
                for (int u = 0; u <= t; u++) {
                    num ds = p[u] * (dp[u] - (num)dot) * scale;
                    if (ds == 0) continue;
                    const num *k = a->k[l][u] + h * HD;
                    num *dk = m->dk[u] + h * HD;
                    for (int j = 0; j < HD; j++) { dq[j] += ds * k[j]; dk[j] += ds * q[j]; }
                }
            }
        }
        memset(m->dh, 0, sizeof m->dh);
        for (int t = 0; t < n; t++) {
            linear_bwd(L->wq, G->wq, D, D, a->h1[l][t], m->dq[t], m->dh[t]);
            linear_bwd(L->wk, G->wk, D, D, a->h1[l][t], m->dk[t], m->dh[t]);
            linear_bwd(L->wv, G->wv, D, D, a->h1[l][t], m->dv[t], m->dh[t]);
        }
        for (int t = 0; t < n; t++) rmsnorm_bwd(a->x[l][t], a->rs1[l][t], L->ln1, m->dh[t], m->dx[t], G->ln1);
    }
    for (int t = 0; t < n; t++) {
        num *de = m->G.tok + (long)ids[t] * D, *dp = m->G.pos + t * D;
        for (int i = 0; i < D; i++) { de[i] += m->dx[t][i]; dp[i] += m->dx[t][i]; }
    }
}

double llm_loss_of(struct llm *m, const int *ids, int n)
{
    forward(m, ids, n, 0);
    return loss_and_probs(m, ids, n);
}

void llm_grad_of(struct llm *m, const int *ids, int n)
{
    memset(m->g, 0, (size_t)NPARAMS * sizeof(num));
    forward(m, ids, n, 0);
    loss_and_probs(m, ids, n);
    backward(m, ids, n);
}

num *llm_weights(struct llm *m, long *count) { if (count) *count = NPARAMS; return m->w; }
num *llm_grads(struct llm *m) { return m->g; }

/* ---- training */

static void adam(struct llm *m)
{
    double norm = 0;
    for (long i = 0; i < NPARAMS; i++) norm += (double)m->g[i] * m->g[i];
    norm = sqrt(norm);
    num clip = norm > CLIP ? (num)(CLIP / norm) : 1;
    m->adam_t++;
    const double b1 = 0.9, b2 = 0.99, eps = 1e-8;
    double c1 = 1.0 - pow(b1, (double)m->adam_t), c2 = 1.0 - pow(b2, (double)m->adam_t);
    for (long i = 0; i < NPARAMS; i++) {
        num g = m->g[i] * clip;
        m->am[i] = (num)(b1 * m->am[i] + (1 - b1) * g);
        m->av[i] = (num)(b2 * m->av[i] + (1 - b2) * g * g);
        double mh = m->am[i] / c1, vh = m->av[i] / c2;
        m->w[i] -= (num)(LR * mh / (sqrt(vh) + eps));
    }
}

int llm_step(struct llm *m, double recent)
{
    if (m->nex == 0) return 0;
    int i;
    if (rnd01(m) < recent) i = (m->ex_next + EX_MAX - 1) % EX_MAX;
    else i = (int)(rnd(m) % (uint64_t)m->nex);
    if (m->nex < EX_MAX) i %= m->nex;        /* the ring is not full: slots 0..nex-1 */
    memset(m->g, 0, (size_t)NPARAMS * sizeof(num));
    forward(m, m->ex[i], m->exlen[i], 0);
    double loss = loss_and_probs(m, m->ex[i], m->exlen[i]);
    backward(m, m->ex[i], m->exlen[i]);
    adam(m);
    m->loss_avg = m->steps == 0 ? loss : 0.97 * m->loss_avg + 0.03 * loss;
    m->steps++;
    m->since_new++;
    m->unsaved++;
    return 1;
}

int llm_wants_review(const struct llm *m)
{
    if (m->nex == 0) return 0;
    long limit = (long)REVIEW_PER_EXAMPLE * m->nex;
    if (limit > REVIEW_MAX) limit = REVIEW_MAX;
    return m->since_new < limit;
}

int llm_train(struct llm *m, int budget_ms)
{
    long end = now_ms() + budget_ms;
    int done = 0;
    while (llm_wants_review(m) && llm_step(m, 0.1)) {
        done++;
        if (now_ms() >= end) break;
    }
    return done;
}

long   llm_steps(const struct llm *m) { return m->steps; }
double llm_loss(const struct llm *m) { return m->loss_avg; }
int    llm_vocab(const struct llm *m) { return m->nvocab - NSPECIAL; }
long   llm_params(void) { return NPARAMS; }
long   llm_unsaved(const struct llm *m) { return m->unsaved; }

int llm_ready(const struct llm *m)
{
    return m->nex >= READY_EXAMPLES && m->steps >= READY_STEPS && m->loss_avg < READY_LOSS;
}

/* ---- generation */

#define TOPK 16

static int sample(struct llm *m, const num *logits, double temperature)
{
    int best[TOPK];
    double val[TOPK];
    int k = 0;
    for (int v = NSPECIAL; v < m->nvocab; v++) {        /* PAD/BOS/SEP are never said */
        double s = logits[v] / (temperature > 0.05 ? temperature : 0.05);
        if (k < TOPK) { best[k] = v; val[k] = s; k++; continue; }
        int lo = 0;
        for (int j = 1; j < TOPK; j++) if (val[j] < val[lo]) lo = j;
        if (s > val[lo]) { best[lo] = v; val[lo] = s; }
    }
    {   /* EOS is allowed too */
        double s = logits[TOK_EOS] / (temperature > 0.05 ? temperature : 0.05);
        if (k < TOPK) { best[k] = TOK_EOS; val[k] = s; k++; }
        else { int lo = 0; for (int j = 1; j < TOPK; j++) if (val[j] < val[lo]) lo = j; if (s > val[lo]) { best[lo] = TOK_EOS; val[lo] = s; } }
    }
    if (k == 0) return TOK_EOS;
    double mx = val[0];
    for (int j = 1; j < k; j++) if (val[j] > mx) mx = val[j];
    double sum = 0;
    for (int j = 0; j < k; j++) { val[j] = exp(val[j] - mx); sum += val[j]; }
    double r = rnd01(m) * sum;
    for (int j = 0; j < k; j++) { r -= val[j]; if (r <= 0) return best[j]; }
    return best[k - 1];
}

int llm_generate(struct llm *m, const char *prompt, double temperature, int max_tokens, char *out, size_t cap)
{
    out[0] = '\0';
    if (!llm_ready(m)) return 0;
    int ids[T];
    int n = 0;
    ids[n++] = TOK_BOS;
    n += tokens_tail(m, prompt ? prompt : "", 0, ids + n, PROMPT_T);
    ids[n++] = TOK_SEP;
    int start = n;
    forward(m, ids, n, 0);
    while (n < T && n - start < max_tokens) {
        int tok = sample(m, m->a->logits[n - 1], temperature);
        if (tok == TOK_EOS) break;
        ids[n++] = tok;
        if (n - start >= 4 && ids[n - 1] == ids[n - 2] && ids[n - 2] == ids[n - 3] && ids[n - 3] == ids[n - 4]) { n -= 2; break; }
        if (n < T) forward(m, ids, n, n - 1);
    }
    size_t o = 0;
    for (int i = start; i < n; i++) {
        char u[4];
        size_t k = utf8_encode(m->vocab[ids[i]], u);
        if (o + k + 1 > cap) break;
        memcpy(out + o, u, k);
        o += k;
    }
    out[o] = '\0';
    return o > 0;
}

/* ---- the file */

#define MAGIC "ALTEREGO-LLM1\n"

struct header {
    char magic[16];
    int32_t d, layers, heads, ff, t, vocab_cap, nvocab;
    int64_t steps, since_new, nex;
    double loss_avg;
};

unsigned char *llm_save(const struct llm *m, size_t *len)
{
    struct header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, MAGIC, sizeof MAGIC);
    h.d = D; h.layers = NL; h.heads = NH; h.ff = FF; h.t = T; h.vocab_cap = V; h.nvocab = m->nvocab;
    h.steps = m->steps; h.since_new = m->since_new; h.nex = m->nex; h.loss_avg = m->loss_avg;
    size_t vb = (size_t)m->nvocab * sizeof(uint32_t), wb = (size_t)NPARAMS * sizeof(float);
    size_t total = sizeof h + vb + wb;
    unsigned char *buf = malloc(total);
    if (!buf) return NULL;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, m->vocab, vb);
    float *w = (float *)(buf + sizeof h + vb);
    for (long i = 0; i < NPARAMS; i++) w[i] = (float)m->w[i];
    ((struct llm *)m)->unsaved = 0;
    *len = total;
    return buf;
}

int llm_load(struct llm *m, const unsigned char *data, size_t len)
{
    struct header h;
    if (len < sizeof h) return -1;
    memcpy(&h, data, sizeof h);
    if (memcmp(h.magic, MAGIC, sizeof MAGIC) != 0 || h.d != D || h.layers != NL || h.heads != NH ||
        h.ff != FF || h.t != T || h.vocab_cap != V || h.nvocab < NSPECIAL || h.nvocab > V)
        return -1;
    size_t vb = (size_t)h.nvocab * sizeof(uint32_t), wb = (size_t)NPARAMS * sizeof(float);
    if (len != sizeof h + vb + wb) return -1;
    /* the vocabulary of the file comes first; code points seen since (the
     * examples added before the load) are re-added after it */
    uint32_t seen[V];
    int nseen = m->nvocab;
    memcpy(seen, m->vocab, (size_t)nseen * sizeof *seen);
    map_clear(m);
    memcpy(m->vocab, data + sizeof h, vb);
    m->nvocab = h.nvocab;
    for (int i = NSPECIAL; i < m->nvocab; i++) map_put(m, m->vocab[i], i);
    const float *w = (const float *)(data + sizeof h + vb);
    for (long i = 0; i < NPARAMS; i++) m->w[i] = (num)w[i];
    memset(m->am, 0, (size_t)NPARAMS * sizeof(num));
    memset(m->av, 0, (size_t)NPARAMS * sizeof(num));
    m->adam_t = 0;
    m->steps = h.steps;
    m->loss_avg = h.loss_avg;
    m->since_new = h.nex == m->nex ? h.since_new : 0;
    m->unsaved = 0;
    /* re-tokenize the examples: ids may have moved */
    int remap[V];
    for (int i = 0; i < NSPECIAL; i++) remap[i] = i;
    for (int i = NSPECIAL; i < nseen; i++) remap[i] = token_of(m, seen[i], 1);
    for (int e = 0; e < m->nex; e++) {
        int n = 0;
        for (int j = 0; j < m->exlen[e]; j++) {
            int id = m->ex[e][j] < nseen ? remap[m->ex[e][j]] : -1;
            if (id >= 0) m->ex[e][n++] = id;
        }
        m->exlen[e] = n;
    }
    return 0;
}

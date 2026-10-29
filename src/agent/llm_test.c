/* Tests of the language model: the gradients against finite differences
 * (built with LLM_NUM=double), training that lowers the loss on a tiny
 * conversation, a save/load round trip, generation that says a learned
 * line. make -C src/agent test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const talk[] = {
    "こんにちは", "今日はいい天気だね", "コーヒーが好き", "明日は会議がある", "ねえ、元気？",
    "カレーが食べたい", "散歩に行こうよ", "猫がかわいい", "眠くなってきた", "またね",
    "hello there", "i like ramen", "see you tomorrow", NULL
};

static void gradient_check(void)
{
    struct llm *m = llm_new(7);
    int ids[LLM_T];
    int n = llm_encode(m, "こんにちは", "今日はいい天気だね", ids);
    CHECK(n == 1 + 5 + 1 + 9 + 1, "encode length %d", n);
    llm_grad_of(m, ids, n);
    long count;
    num *w = llm_weights(m, &count);
    num *g = llm_grads(m);
    /* copy: the backward of the next loss_of calls would not touch g, but be safe */
    num *gc = malloc((size_t)count * sizeof *gc);
    memcpy(gc, g, (size_t)count * sizeof *gc);
    /* a spread of weights: embeddings of used tokens, positions, every matrix, the norms */
    long picks[64];
    int np = 0;
    long tok_base = 0, pos_base = (long)LLM_VOCAB * LLM_D, layer_base = pos_base + (long)LLM_T * LLM_D;
    long layer_size = LLM_D + 4L * LLM_D * LLM_D + LLM_D + 2L * LLM_FF * LLM_D;
    for (int i = 0; i < 6; i++) picks[np++] = tok_base + (long)ids[i] * LLM_D + i * 7;
    for (int i = 0; i < 4; i++) picks[np++] = pos_base + i * 37;
    for (int l = 0; l < LLM_LAYERS; l++) {
        long b = layer_base + l * layer_size;
        picks[np++] = b + 3;                                   /* ln1 */
        picks[np++] = b + LLM_D + 11;                          /* wq */
        picks[np++] = b + LLM_D + LLM_D * LLM_D + 200;         /* wk */
        picks[np++] = b + LLM_D + 2 * LLM_D * LLM_D + 777;     /* wv */
        picks[np++] = b + LLM_D + 3 * LLM_D * LLM_D + 1234;    /* wo */
        picks[np++] = b + LLM_D + 4 * LLM_D * LLM_D + 5;       /* ln2 */
        picks[np++] = b + 2 * LLM_D + 4 * LLM_D * LLM_D + 999; /* w1 */
        picks[np++] = b + 2 * LLM_D + 4 * LLM_D * LLM_D + LLM_FF * LLM_D + 4321; /* w2 */
    }
    picks[np++] = count - 1;                                   /* lnf */
    const double h = 1e-5;
    int bad = 0;
    for (int i = 0; i < np; i++) {
        long k = picks[i];
        num saved = w[k];
        w[k] = saved + (num)h;
        double lp = llm_loss_of(m, ids, n);
        w[k] = saved - (num)h;
        double lm = llm_loss_of(m, ids, n);
        w[k] = saved;
        double fd = (lp - lm) / (2 * h);
        double an = gc[k];
        double err = fabs(fd - an) / (fabs(fd) + fabs(an) + 1e-8);
        if (err > 1e-4 && fabs(fd - an) > 1e-7) { bad++; printf("  grad mismatch at %ld: analytic %.9g numeric %.9g\n", k, an, fd); }
    }
    CHECK(bad == 0, "%d of %d gradients wrong", bad, np);
    free(gc);
    llm_free(m);
}

static void training(void)
{
    struct llm *m = llm_new(42);
    for (int i = 1; talk[i]; i++) llm_add_example(m, talk[i - 1], talk[i]);
    CHECK(llm_examples(m) == 12, "examples %d", llm_examples(m));
    CHECK(llm_vocab(m) > 40, "vocab %d", llm_vocab(m));
    CHECK(!llm_ready(m), "ready before training");
    double first = -1;
    for (int s = 0; s < 400; s++) {
        llm_step(m, 0.1);
        if (first < 0) first = llm_loss(m);
    }
    printf("  loss %.3f -> %.3f after %ld steps (%ld weights)\n", first, llm_loss(m), llm_steps(m), llm_params());
    CHECK(llm_loss(m) < first * 0.5, "loss did not fall: %.3f -> %.3f", first, llm_loss(m));
    CHECK(llm_loss(m) < 1.0, "loss still %.3f", llm_loss(m));
    CHECK(llm_ready(m), "not ready after training");
    CHECK(llm_wants_review(m), "should still want review (%ld steps)", llm_steps(m));

    /* a memorized continuation, at a low temperature */
    char out[256];
    int hit = 0;
    for (int tries = 0; tries < 8 && !hit; tries++) {
        if (llm_generate(m, "こんにちは", 0.3, 40, out, sizeof out) && strcmp(out, "今日はいい天気だね") == 0) hit = 1;
    }
    printf("  こんにちは -> %s\n", out);
    CHECK(hit, "did not learn the continuation of こんにちは (got %s)", out);

    /* save, load into a fresh model, same loss and vocabulary */
    size_t len;
    unsigned char *bytes = llm_save(m, &len);
    CHECK(bytes && len > 100000, "save");
    CHECK(llm_unsaved(m) == 0, "unsaved after save");
    struct llm *m2 = llm_new(1);
    for (int i = 1; talk[i]; i++) llm_add_example(m2, talk[i - 1], talk[i]);
    CHECK(llm_load(m2, bytes, len) == 0, "load");
    CHECK(llm_steps(m2) == llm_steps(m) && llm_vocab(m2) == llm_vocab(m), "steps/vocab after load");
    int ids[LLM_T];
    int n = llm_encode(m, "こんにちは", "今日はいい天気だね", ids);
    double l1 = llm_loss_of(m, ids, n);
    int ids2[LLM_T];
    int n2 = llm_encode(m2, "こんにちは", "今日はいい天気だね", ids2);
    double l2 = llm_loss_of(m2, ids2, n2);
    CHECK(fabs(l1 - l2) < 1e-4, "loss differs after load: %.6f vs %.6f", l1, l2);
    CHECK(llm_ready(m2), "loaded model not ready");
    /* an example added before the load, with a code point the file does not know, survives */
    struct llm *m3 = llm_new(2);
    llm_add_example(m3, "またね", "鯨が泳ぐ");
    CHECK(llm_load(m3, bytes, len) == 0, "load 3");
    CHECK(llm_vocab(m3) == llm_vocab(m) + 3, "vocab after load+new chars: %d vs %d", llm_vocab(m3), llm_vocab(m));
    CHECK(llm_step(m3, 1.0) == 1, "step on the re-tokenized example");
    /* a damaged file is refused */
    bytes[20] ^= 1;
    CHECK(llm_load(m3, bytes, len) == -1, "damaged header accepted");
    CHECK(llm_load(m3, bytes, len - 1) == -1, "short file accepted");
    free(bytes);
    llm_free(m3);
    llm_free(m2);
    llm_free(m);
}

static void budget(void)
{
    struct llm *m = llm_new(3);
    CHECK(llm_train(m, 50) == 0, "trained with no examples");
    CHECK(!llm_wants_review(m), "wants review with nothing to review");
    llm_add_example(m, "", "はじめまして");
    int done = llm_train(m, 30);
    CHECK(done >= 1, "no step within the budget");
    printf("  %d steps in 30 ms\n", done);
    char out[64];
    CHECK(llm_generate(m, "a", 1.0, 10, out, sizeof out) == 0 && out[0] == '\0', "spoke before ready");
    llm_free(m);
}

int main(void)
{
    printf("gradient check (%s)\n", sizeof(num) == sizeof(double) ? "double" : "float");
    if (sizeof(num) == sizeof(double)) gradient_check();
    else printf("  skipped: build with LLM_NUM=double\n");
    printf("training\n");
    training();
    printf("budget\n");
    budget();
    printf(fails ? "%d FAILED\n" : "ok\n", fails);
    return fails ? 1 : 0;
}

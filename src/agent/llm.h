#ifndef ALTEREGO_LLM_H
#define ALTEREGO_LLM_H

#include <stddef.h>
#include <stdint.h>

/* AlterEgo's own language model: a small Transformer written here, trained
 * here, run here. Nothing is downloaded and nothing was trained elsewhere:
 * at birth the weights are noise, and every word the user says is one more
 * example it learns from. It works on characters (Unicode code points), so
 * Japanese and English need no dictionary and no tokenizer.
 *
 * An example is a pair of consecutive user utterances, "what was said" ->
 * "what the user said next"; the model learns to continue a conversation
 * the way the user does, and at generation time the user's line is the
 * prompt. It trains on-device, in small steps that fit into the pauses of
 * the conversation (brain.c calls this "reviewing", 復習), and the weights
 * live in the store (model.bin) so the training survives reboots.
 *
 * The size (LLM_D, LLM_LAYERS, ...) is chosen for a machine with no GPU and
 * a QEMU guest: about two hundred thousand weights, a training step in
 * tens of milliseconds on a PC. */

#define LLM_D       64        /* width of the residual stream */
#define LLM_LAYERS  2
#define LLM_HEADS   4
#define LLM_FF      128       /* width of the feed-forward hidden layer */
#define LLM_T       64        /* longest sequence, in tokens */
#define LLM_VOCAB   2048      /* most code points it can tell apart */

#ifndef LLM_NUM
#define LLM_NUM float         /* the test builds it in double for the gradient check */
#endif
typedef LLM_NUM num;

struct llm;

struct llm *llm_new(uint64_t seed);
void        llm_free(struct llm *m);

/* One training example: `prev` was said, then `cur`. Code points not seen
 * before are added to the vocabulary (until it is full). */
void llm_add_example(struct llm *m, const char *prev, const char *cur);
int  llm_examples(const struct llm *m);

/* Training. llm_step does one step on one example (the newest with
 * probability `recent`); llm_train keeps stepping for `budget_ms`
 * milliseconds and returns how many steps it did. llm_wants_review says
 * whether more steps would still help (it stops a while after the last
 * new example, so the machine idles). */
int  llm_step(struct llm *m, double recent);
int  llm_train(struct llm *m, int budget_ms);
int  llm_wants_review(const struct llm *m);

long   llm_steps(const struct llm *m);         /* training steps so far */
double llm_loss(const struct llm *m);          /* recent training loss, nats per character */
int    llm_vocab(const struct llm *m);         /* code points it knows */
long   llm_params(void);                       /* number of weights */
int    llm_ready(const struct llm *m);         /* trained enough to be worth listening to */

/* Continue `prompt` the way the user would: at most `max_tokens` code
 * points into `out`; 1 when there is something, 0 when not ready or empty.
 * `temperature` ~0.6 (safe) .. 1.2 (wild). */
int  llm_generate(struct llm *m, const char *prompt, double temperature, int max_tokens, char *out, size_t cap);

/* The weights and the vocabulary as bytes (malloc'd; *len set), and back.
 * llm_load returns -1 and leaves `m` untouched when the bytes are not a
 * model of this shape. */
unsigned char *llm_save(const struct llm *m, size_t *len);
int  llm_load(struct llm *m, const unsigned char *data, size_t len);
long llm_unsaved(const struct llm *m);         /* steps since llm_save/llm_load */

/* Forward pass for the tests: loss of one example, and its gradient. */
double llm_loss_of(struct llm *m, const int *ids, int n);
void   llm_grad_of(struct llm *m, const int *ids, int n);
num   *llm_weights(struct llm *m, long *count);
num   *llm_grads(struct llm *m);
int    llm_encode(struct llm *m, const char *prev, const char *cur, int *ids);   /* the example's tokens */

#endif

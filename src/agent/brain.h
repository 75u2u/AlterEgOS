#ifndef ALTEREGO_BRAIN_H
#define ALTEREGO_BRAIN_H

#include <stddef.h>

/* AlterEgo's mind. It is raised, not programmed: at the first boot it has
 * no personality, no words and no memories, and everything it becomes
 * comes out of the conversations with the user. No external model, no
 * network. What it keeps:
 *
 *   - language: its own small Transformer language model (llm.c), trained
 *     here on the conversation, which from a few hundred study steps on
 *     answers in the user's words; until then (and when it has nothing to
 *     say) a character n-gram model (orders 1..3) and, for English, a word
 *     n-gram (orders 1..2) of what the user said, plus the log of
 *     utterances, searched for the past line most like the current one;
 *     the user is the only speaker it hears, so the user's words are the
 *     words it learns to speak with,
 *   - a personality: five traits (cheerfulness, politeness, curiosity,
 *     talkativeness, affection), all neutral at birth, pushed a little by
 *     every utterance (how the user talks, praise and scolding),
 *   - memories: facts the user states (likes, dislikes, who they are,
 *     "remember this"), the user's name and its own name, both learned
 *     from the conversation,
 *   - a growth stage from the number of conversations: newborn (echoes a
 *     word), babbling (words and short fragments), chatty (sentences from
 *     the learned language), grown (uses memories and its personality).
 *
 * Everything is rebuilt from the utterance log at start, so the log is the
 * only thing that has to be stored. */

struct brain;
struct persona;

enum brain_lang { BRAIN_JA, BRAIN_EN };
enum brain_lang brain_detect_lang(const char *s, enum brain_lang fallback);
enum brain_lang brain_lang(const struct brain *b);     /* of the latest utterance */

enum brain_trait { T_CHEER, T_POLITE, T_CURIOUS, T_TALK, T_AFFECTION, T_COUNT };

struct brain *brain_new(const int stage_turns[3]);     /* turns to reach stages 1..3 */
void          brain_free(struct brain *b);

void brain_learn(struct brain *b, const char *utterance);     /* one user line */
void brain_load_log(struct brain *b, const char *text);       /* newline-separated */
int  brain_turns(const struct brain *b);
int  brain_stage(const struct brain *b);                      /* 0..3 */
int  brain_trait(const struct brain *b, enum brain_trait t);  /* -100..100 */
int  brain_memories_count(const struct brain *b);

/* Names learned in conversation, or the persona's defaults. */
const char *brain_user_name(const struct brain *b, const struct persona *p);
const char *brain_own_name(const struct brain *b, const struct persona *p);

/* The body's condition (body.h level 0..3): colours 「元気？」 and the status. */
void brain_set_body(struct brain *b, int level);

/* True for lines that ask about the mind itself (status, memories): they
 * are answered but not learned as utterances. */
int brain_is_query(const char *line);

/* Compose the reply to `input` (learn it first). */
void brain_reply(struct brain *b, const struct persona *p, const char *input, char *out, size_t cap);
/* Growth, personality and names, in words, in the latest language. */
void brain_status(const struct brain *b, const struct persona *p, char *out, size_t cap);
/* Keep a note of something that happened (「〇〇の絵を見せてもらった」). */
void brain_remember_note(struct brain *b, const char *text);
/* What it remembers, one fact per line. */
void brain_memories(const struct brain *b, const struct persona *p, char *out, size_t cap);

/* Closeness to the user (0..100) and its name (初対面 … 家族同然), the
 * short-lived mood (-100..100). Both grow out of the conversation and are
 * shown in the status. */
int  brain_intimacy(const struct brain *b);
int  brain_mood(const struct brain *b);
const char *brain_intimacy_word(const struct brain *b);
/* Something it says on its own into a silence (a question, a topic);
 * 1 when `out` has a line. Ask once per silence. */
int  brain_idle_line(struct brain *b, const struct persona *p, char *out, size_t cap);
/* A line for the greeting about the time apart ("3 日ぶりだね") or the last
 * topic; "" when there is nothing to say. */
void brain_welcome(struct brain *b, const struct persona *p, char *out, size_t cap);
/* The part of the mind the utterance log cannot rebuild (bond.txt):
 * intimacy, mood, which questions were asked and what was answered, how
 * many boots, when it last heard the user. Load after the log. */
void brain_bond_load(struct brain *b, const char *text, long now);
void brain_bond_save(const struct brain *b, long now, char *out, size_t cap);

/* Its own language model (llm.c), trained on the conversation, on this
 * machine. brain_review studies in a pause for up to `budget_ms`
 * milliseconds and returns the steps it did (0 when there is nothing new
 * left to study: brain_wants_review). The weights are the one part of the
 * mind that is slow to rebuild, so they are saved (model.bin): save returns
 * malloc'd bytes, or NULL when little changed and not `force`. */
int  brain_review(struct brain *b, int budget_ms);
int  brain_wants_review(const struct brain *b);
int  brain_model_load(struct brain *b, const unsigned char *data, size_t len);
unsigned char *brain_model_save(struct brain *b, size_t *len, int force);
int  brain_speaks_itself(const struct brain *b);      /* the model is trained enough to answer */

#endif

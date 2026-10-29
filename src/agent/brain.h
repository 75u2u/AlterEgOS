#ifndef ALTEREGO_BRAIN_H
#define ALTEREGO_BRAIN_H

#include <stddef.h>

/* AlterEgo's mind. It is raised, not programmed: at the first boot it has
 * no personality, no words and no memories, and everything it becomes
 * comes out of the conversations with the user. No external model, no
 * network. What it keeps:
 *
 *   - language: a character n-gram model (orders 1..3) and, for English, a
 *     word n-gram (orders 1..2) of what the user said, plus the log of
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

#endif

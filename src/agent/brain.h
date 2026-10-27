#ifndef ALTEREGO_BRAIN_H
#define ALTEREGO_BRAIN_H

#include <stddef.h>

/* AlterEgo's own conversational engine. No external model, no network: it
 * learns from what "わたし" (the user) says and answers out of that.
 *
 *   - a character n-gram model (orders 1..3) of the user's utterances, used
 *     to generate text in the user's own style,
 *   - the log of utterances, searched for the past line most like the
 *     current one: its successor, what the user said next back then, is
 *     the user's own answer, so it becomes AlterEgo's,
 *   - frequency tables of "words" (runs of kanji / kana / latin) and of
 *     sentence endings: the habits (口癖) and topics,
 *   - a mirror ratio that grows with the number of utterances: early on
 *     AlterEgo answers in its own polite voice, later more and more in the
 *     user's (self-projection).
 *
 * Everything is rebuilt from the utterance log at start, so the log is the
 * only thing that has to be stored. */

struct brain;
struct persona;

struct brain *brain_new(int mirror_after);           /* turns until fully mirrored */
void          brain_free(struct brain *b);

void brain_learn(struct brain *b, const char *utterance);     /* one user line */
void brain_load_log(struct brain *b, const char *text);       /* newline-separated */
int  brain_turns(const struct brain *b);
int  brain_mirror_percent(const struct brain *b);

/* Compose the reply to `input` (already learned or not; call learn first
 * so the reply can use it). */
void brain_reply(struct brain *b, const struct persona *p, const char *input,
                 char *out, size_t cap);

/* The "わたし像": what has been learned, in words. */
void brain_self_model(const struct brain *b, const struct persona *p, char *out, size_t cap);

#endif

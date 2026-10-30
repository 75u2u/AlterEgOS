#ifndef ALTEREGO_PLAY_H
#define ALTEREGO_PLAY_H

#include <stddef.h>
#include <stdint.h>

#include "brain.h"

/* Things to do together that need no learning, so that even a newborn
 * AlterEgo has a repertoire: じゃんけん (rock-paper-scissors, with a running
 * score), a dice and a coin, 数当て (guess my number), しりとり (the word
 * chain game), なぞなぞ (riddles), おみくじ (a fortune), ダジャレ (a pun),
 * counting, a song, and sums (3+4は？). A game that waits for the user
 * (a hand, a number, a word, an answer) keeps its state here between
 * lines; 「やめる」 ends it. */

enum play_mode { PLAY_NONE = 0, PLAY_JANKEN, PLAY_GUESS, PLAY_SHIRITORI, PLAY_RIDDLE };

#define PLAY_USED_MAX 48

struct play {
    enum play_mode mode;
    int wins, losses, draws;        /* じゃんけん, AlterEgo's point of view; kept in bond.txt */
    int secret, tries, lo, hi;      /* 数当て */
    char last[32];                  /* しりとり: the word AlterEgo said last (hiragana) */
    char used[PLAY_USED_MAX][32];   /* ... and every word said so far */
    int nused;
    int riddle, hints;              /* なぞなぞ: the open riddle */
    int rounds;                     /* games played in this life */
};

/* True when `line` belongs to a game: one is waiting for it, or it starts
 * one (「じゃんけん」, 「サイコロ振って」, 「3+4は？」...). Such a line is
 * played, not learned. `lo` is `line` with ASCII lowercased. */
int play_wants(const struct play *g, const char *line, const char *lo);

/* Play the line: 1 when `out` has the reply. `style` is 0 plain, 1 polite,
 * 2 casual; `stage` 0 is the newborn, who plays in fewer words. `rng` is
 * the mind's random state (xorshift64). `expr` gets what it felt (happy,
 * sad, surprised, curious, "" for nothing special) and `mood` how much
 * the result moved it. */
int play_reply(struct play *g, enum brain_lang lang, int stage, int style, const char *you,
               const char *line, const char *lo, uint64_t *rng, char *out, size_t cap,
               char *expr, size_t ecap, int *mood);

#endif

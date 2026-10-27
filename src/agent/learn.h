#ifndef ALTEREGO_LEARN_H
#define ALTEREGO_LEARN_H

/* Self-projection, first step. AlterEgo learns the user ("わたし") a little
 * on every turn and projects what it learned onto itself. Before the model
 * and the memory store exist (days 2-3) the only thing it can observe is the
 * raw text, so this learns the one habit visible in raw text: how the user
 * ends sentences (口癖). With the model wired in, the real learning is the
 * self-model text below, rewritten after every turn. */

#define LEARN_ENDING_LEN 16      /* bytes; up to 3 UTF-8 code points */
#define LEARN_TABLE      32

#define LEARN_SELF_MAX   4096    /* bytes of self-model text */

struct learn {
    struct { char ending[LEARN_ENDING_LEN]; int count; } habit[LEARN_TABLE];
    int turns;
    /* The "わたし像": what the model has written down about the user so far
     * (personality, speech habits, values). Rewritten after every turn by
     * the reflection call, kept in the store when there is one. */
    char self_model[LEARN_SELF_MAX];
};

void        learn_init(struct learn *l);
void        learn_observe(struct learn *l, const char *line);   /* one user turn */
/* The most frequent sentence ending seen at least twice, or NULL. */
const char *learn_habit(const struct learn *l, int *count);

#endif

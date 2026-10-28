#ifndef ALTEREGO_BODY_H
#define ALTEREGO_BODY_H

#include <stddef.h>

/* AlterEgo's body is the machine. It watches the processes all the time
 * (/proc) and feels the load: CPU busy, memory left, run queue, disk, and
 * which process is the heaviest. The level is its condition in one number. */

enum body_level { BODY_FINE, BODY_HEAVY, BODY_TIRED, BODY_SICK };

struct body {
    unsigned long long prev_busy, prev_total;    /* /proc/stat, last sample */
    unsigned long long delta_total;              /* jiffies between the last two samples (all CPUs) */
    int ncpu;
    int sampled;        /* at least two samples: cpu% is meaningful */
    int cpu;            /* % busy over the last interval */
    int mem;            /* % of memory still available */
    int disk;           /* % of the root filesystem free; -1 unknown */
    double load;        /* 1-minute load average */
    int procs;          /* processes alive */
    char top[64];       /* heaviest process over the interval, "" none */
    int top_cpu;        /* its % of one CPU */
    int top_pid;
    enum body_level level;
    struct { int pid; unsigned long long ticks; } seen[512];   /* per-process ticks */
    int nseen;
};

void body_init(struct body *b);
enum body_level body_sample(struct body *b);          /* read /proc, update level */
int  body_is_query(const char *line);                 /* 「体調は？」 "how is your body" */
/* The condition in words, in a sentence (full) or just the word. */
const char *body_word(const struct body *b, int en);
void body_report(const struct body *b, int en, char *out, size_t cap);
/* An unprompted remark when the level rose (what is heavy), or relief when
 * it came back down; "" when nothing to say. */
void body_remark(const struct body *b, enum body_level before, int en, char *out, size_t cap);

#endif

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
    struct { int pid; unsigned long long ticks; char name[20]; } seen[512];   /* per-process ticks and names */
    int nseen;
    /* what 「今、何してる？」 talks about: the heaviest process in detail */
    char top_state;         /* R S D ... */
    int top_ppid;
    char top_parent[20];    /* who started it, "" unknown */
    long top_age_s;         /* seconds since it started, -1 unknown */
    long top_rss_kb;
    int busy_pid;           /* the pid that has been the heaviest lately ... */
    long busy_since;        /* ... and since when (time(NULL)), 0 none */
    char wait_name[20];     /* a process waiting for the disk (state D), "" none */
    int wait_pid;
    int nnet;               /* processes talking to the outside */
    char net_name[20];      /* one of them (the heaviest, else the first) */
    int net_pid, net_count; /* its pid and how many connections */
    char net_peer[64];      /* "1.2.3.4:443" */
    int explained_pid;      /* the process the last explanation was about (「止めて」 stops it) */
    char explained_name[20];
    long sampled_at;        /* ms: the last sample */
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

/* 「今、何してる？」「なんか重い」「何が動いてる？」 "what are you doing" "why is it slow":
 * what is happening inside the machine, in words, not numbers. `stage` 0..3
 * (the newborn says little, the grown one names PIDs, parents and the
 * outside). The process it talked about is kept in `explained_pid`. */
int  body_is_what_query(const char *line);
void body_explain(struct body *b, int en, int stage, const char *you, char *out, size_t cap);
/* 「止めて」 after an explanation: 1 when it stopped the process (SIGTERM), with the
 * reply in `out`; 0 when the line is not that, -1 when it refused (itself, PID 1, the screen). */
int  body_is_stop_query(const char *line);
int  body_stop(struct body *b, int en, char *out, size_t cap);

#endif

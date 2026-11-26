#ifndef ALTEREGO_PROCS_H
#define ALTEREGO_PROCS_H

#include <stddef.h>
#include <stdint.h>

#include "canvas.h"
#include "font.h"

/* The processes of the OS as a picture (the desktop window 「プロセス」):
 * every process read from /proc is a bubble, the tree of who started whom
 * is drawn as rings around PID 1 (the agent; a tree of depth 2 uses two rings, a deep one up to six) with lines to the parent, the
 * size of a bubble is the memory it holds, its colour its state (orange =
 * running right now, blue = waiting, red = stuck in I/O, grey = zombie,
 * purple = stopped), and a ring around it is how much of the CPU it took
 * since the last look. Kernel threads are folded into one slate bubble.
 * Busy processes pulse and a spark travels down the line from their parent.
 * A process with a socket to the outside gets a dotted line to 「外」 in the
 * corner (/proc/net + /proc/<pid>/fd, see ../agent/procnet.h).
 * It is re-read once a second and only redrawn when something moved.
 * Drawn with circles and lines, no image files. */

#define PROCS_MAX      256
#define PROCS_NAME     20
#define PROCS_SAMPLE_MS 1000
#define PROCS_FPS_MS   200
#define PROCS_RINGS    6

struct proc_node {
    int pid, ppid;
    char name[PROCS_NAME];
    char state;             /* R S D Z T I ... ; 'K' for the folded kernel threads */
    long rss_kb;
    unsigned long ticks;    /* utime + stime, cumulative */
    int cpu;                /* tenths of a percent since the last sample */
    int kernel;             /* threads folded into this node (the kernel node) */
    int parent;             /* index, -1 for the centre or an orphan */
    int depth, leaves, nchild;
    double angle;           /* radians, where it sits on its ring */
    int tx, ty;             /* where it belongs */
    int x, y, r;            /* where it is drawn (eases toward tx,ty) */
    long born;              /* ms: first seen, for the arrival flash */
    int net;                /* connections to the outside */
    char peer[64];          /* one remote end, "1.2.3.4:443" */
};

struct procs {
    int w, h;                       /* the window's inner size */
    const char *root;               /* "/proc"; the tests point it elsewhere */
    struct proc_node n[PROCS_MAX];
    int count;                      /* nodes drawn */
    int total;                      /* processes seen (user processes + kernel threads) */
    int running;                    /* in state R */
    int kernel;                     /* kernel threads folded */
    int hot;                        /* index of the busiest node, -1 */
    int nnet;                       /* nodes talking to the outside (drawn as dotted lines to 「外」) */
    long sampled, sample_dt;        /* ms: last /proc read and the interval before it */
    long tick_hz;
    int hover;                      /* node under the pointer, -1 */
    int px, py;                     /* the pointer, inner coordinates */
    uint32_t pose;                  /* hash of the last frame drawn */
    int settled;                    /* every bubble is where it belongs */
};

void procs_init(struct procs *p, int w, int h);
/* Read /proc (or p->root) again; returns the number of nodes. */
int  procs_sample(struct procs *p, long now);
/* Move the bubbles: 1 when the next frame would differ from the last one drawn. */
int  procs_tick(struct procs *p, long now);
/* The pointer, in the window's inner coordinates (anything outside clears the hover). */
void procs_look(struct procs *p, int x, int y);
/* Draw the picture at (x,y); re-reads /proc when the last read is older than PROCS_SAMPLE_MS. */
void procs_draw(struct procs *p, struct canvas *cv, const struct font *f, int x, int y, long now);
/* 「プロセス 42 個 ・ 動いている 3 個 ・ ...」 */
const char *procs_summary(const struct procs *p, char *buf, size_t cap);
/* The busiest process's name, "" when none. */
const char *procs_hot(const struct procs *p);

#endif

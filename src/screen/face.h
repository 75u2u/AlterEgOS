#ifndef ALTEREGO_FACE_H
#define ALTEREGO_FACE_H

#include <stdint.h>

#include "canvas.h"
#include "font.h"

/* AlterEgo's character: a face the desktop draws and animates, so that
 * what goes on in the agent shows on the screen as it happens. The agent
 * sends its state after every line it says (an OSC string
 * "7700;face;mood=…;bond=…;stage=…;body=…;expr=…;say=…"); the desktop
 * keeps the face alive between those: it blinks, breathes, follows the
 * mouse with its eyes, moves its mouth while the words come out, looks up
 * when the user types, gets drowsy and falls asleep when left alone, and
 * grows with the agent's stage. Only the head is shown. The look (model)
 * is the user's choice, told by the agent: 0 = a small friendly robot
 * whose eyes and mouth are light on a face screen, 1 = a maid android
 * with cat ears and an intercom headset. face_tick says whether the next frame would differ from the
 * last one drawn, so that the desktop redraws only when something moved. The expression (happy, sad, shy, curious, puzzled, surprised,
 * nod, love, angry, tired, dizzy, sleep, drowsy, listen, calm) is either
 * what the agent just said it feels, for a few seconds, or what its mood,
 * its body and the silence say. */

#define FACE_W 240
#define FACE_H 200

struct face {
    int mood;                   /* -100..100 */
    int bond;                   /* 0..100 */
    int stage;                  /* 0..3 */
    int body;                   /* 0 fine .. 3 struggling */
    int model;                  /* the look: 0 robot, 1 maid android */
    char expr[16];              /* what the agent said it feels, "" = derive */
    long expr_until;            /* ms: how long that holds */
    long say_until;             /* ms: the mouth moves until then */
    long last_heard;            /* ms: the agent last spoke */
    long last_key;              /* ms: the user last typed */
    long blink_at;              /* ms: the current blink started */
    long blink_next;            /* ms: the next one */
    int look_x, look_y;         /* where the eyes look, -4..4 */
    int target_x, target_y;
    uint32_t rng;
    uint32_t pose;              /* a hash of what was last drawn (face_tick compares) */
    char label[32];             /* the word under the face, after face_draw */
};

void face_init(struct face *f, long now);
/* The agent's state, the list after "7700;face;" (keys mood, bond, stage,
 * body, expr, say = code points just said). */
void face_set(struct face *f, const char *kv, long now);
void face_typing(struct face *f, long now);                     /* the user pressed a key in the chat */
void face_look(struct face *f, int dx, int dy);                 /* the pointer, relative to the face's centre */
int  face_tick(struct face *f, long now);                       /* advance the animation; 1 if the face changed */
/* The expression it shows at `now` (one of the names above). */
const char *face_expr(const struct face *f, long now);
int  face_talking(const struct face *f, long now);
void face_draw(struct face *f, struct canvas *cv, const struct font *font, int x, int y, long now);

#endif

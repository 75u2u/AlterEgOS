#ifndef ALTEREGO_SKILLS_H
#define ALTEREGO_SKILLS_H

#include <stddef.h>
#include "brain.h"

/* AlterEgo's hands: what it can do to the OS it is, asked for in
 * conversation, and how that set of abilities grows.
 *
 *   - built in: the time, listing / reading / writing / making / removing
 *     files and directories, the processes, memory and disk, running a
 *     shell command ("実行して: ..."), rebooting and powering off, opening
 *     and closing the desktop's windows (apps.c: 「ペイントを開いて」,
 *     「https://… を開いて」, 「〇〇を調べて」) and looking at a picture
 *     drawn in paint (「絵を見て」; what it saw is remembered, `b`),
 *   - learned: "「X」と言ったら「Y」を実行して" / "when I say X, run Y" adds
 *     an ability X whose action Y is either another ability (composition)
 *     or a shell command. Kept in skills.txt in the store, so they survive
 *     a reboot and the OS grows new functions out of the conversation,
 *   - every action is appended to actions.log (when, asked how, what ran,
 *     how it ended), and "何をした？" / "what did you do" reads it back.
 *
 * The agent is root and asks no confirmation (the design); the log and
 * the snapshots are the safety net. */

struct skills;

struct skills *skills_new(void);             /* loads skills.txt when a store is open */
void           skills_free(struct skills *s);
int            skills_count(const struct skills *s);

/* True when `line` asks for an ability (built in or learned): then it is
 * acted on, and not learned as a word. `out` receives the reply, possibly
 * several lines. `me` / `you` are the names to speak with. */
int skills_handle(struct skills *s, struct brain *b, enum brain_lang lang, const char *me, const char *you,
                  const char *line, char *out, size_t cap);

#endif

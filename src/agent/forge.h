#ifndef ALTEREGO_FORGE_H
#define ALTEREGO_FORGE_H

#include <stddef.h>

/* The forge: when AlterEgo is asked for something it has no ability for,
 * it builds one on the spot, out of parts it owns (the small tools of its
 * own userland: find, grep, wc, sort, du, sed...), runs it, and keeps it.
 *
 *   「/tmp/x の中の txt ファイルを数えて」
 *     → slots: {path}=/tmp/x {ext}=txt; part: count files
 *     → find '/tmp/x' -maxdepth 1 -type f -name '*.txt' | wc -l
 *     → the sentence with its slots blanked becomes the trigger of a new
 *       ability: 「{path} の中の {ext} ファイルを数えて」, kept in skills.txt
 *       with the command template, so 「/home の中の log ファイルを数えて」
 *       works the next time without building anything.
 *
 * No model is involved: the parts are a table, the slots are found by
 * their shape (a path, a quoted word, a number, an extension), and the
 * sentence is matched by its words. 「違う」 tries the next part that fits. */

#define FORGE_PATH 256
#define FORGE_WORD 128
#define FORGE_PAT  160          /* a trigger pattern (also skills.c's TRIG_MAX) */
#define FORGE_CMD  256          /* a command template (skills.c's ACT_MAX) */

struct forge_slots {
    char path[FORGE_PATH], path2[FORGE_PATH];   /* /…, ~/…, ./… in order */
    char word[FORGE_WORD], word2[FORGE_WORD];   /* 「…」 or "…" in order */
    char ext[16];                               /* .txt or "txt ファイル" */
    long n;                                     /* a number, ASCII or fullwidth */
    int have_path, have_path2, have_word, have_word2, have_ext, have_n;
};

struct forge_plan {
    char pattern[FORGE_PAT];    /* the sentence with its slots blanked */
    char cmd[FORGE_CMD];        /* the command template */
    char said[96];              /* the part, in words (ja or en) */
    int count;                  /* parts that fit the sentence (0: cannot build) */
    int need_path;              /* a part fits but the sentence names no place */
};

/* Does the sentence ask for a thing to be done to files / words / lines
 * (so that a missing ability is worth building)? */
int  forge_is_request(const char *line, const char *lo);
/* Find the slots in a sentence; `pattern` receives the sentence with them blanked. */
int  forge_slots(const char *line, struct forge_slots *sl, char *pattern, size_t cap);
/* Pick the `alt`-th part that fits; returns p->count. */
int  forge_plan(const char *line, const char *lo, int en, int alt, struct forge_plan *p);
/* Match a blanked pattern against a new sentence, filling the slots. */
int  forge_match(const char *pattern, const char *line, struct forge_slots *sl);
/* Put the slots into a template ({path} and {word} shell-quoted, {ext} and
 * {n} raw, {n|5} with a default, {word:sed} escaped for sed). 0 when a
 * needed slot is missing. */
int  forge_fill(const char *tmpl, const struct forge_slots *sl, char *out, size_t cap);
/* The parts, listed for 「それはまだ作れない」. */
void forge_parts(int en, char *out, size_t cap);

#endif

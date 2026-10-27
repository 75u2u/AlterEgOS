#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "learn.h"

void learn_init(struct learn *l)
{
    memset(l, 0, sizeof *l);
}

static int is_utf8_cont(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/* Drop trailing spaces and sentence punctuation (ASCII and the Japanese
 * full-width ones) so "疲れたなあ。" and "疲れたなあ!" share an ending. */
static size_t strip_tail(const char *s, size_t n)
{
    static const char *const punct[] = { "。", "、", "！", "？", "…", "〜", "ー", NULL };
    for (;;) {
        if (n == 0) return 0;
        if (strchr(" \t\r.,!?", s[n - 1]) && s[n - 1] != '\0') { n--; continue; }
        int cut = 0;
        for (int i = 0; punct[i]; i++) {
            size_t pl = strlen(punct[i]);
            if (n >= pl && memcmp(s + n - pl, punct[i], pl) == 0) { n -= pl; cut = 1; break; }
        }
        if (!cut) return n;
    }
}

/* Byte offset where the last `points` code points of s[0..n) start. */
static size_t last_points(const char *s, size_t n, int points)
{
    size_t i = n;
    while (i > 0 && points > 0) {
        i--;
        while (i > 0 && is_utf8_cont((unsigned char)s[i])) i--;
        points--;
    }
    return i;
}

void learn_observe(struct learn *l, const char *line)
{
    l->turns++;
    size_t n = strip_tail(line, strlen(line));
    if (n == 0) return;
    size_t start = last_points(line, n, 2);
    if (start == 0) return;                 /* the whole line; not a habit, just a word */
    size_t len = n - start;
    if (len >= LEARN_ENDING_LEN) return;
    char ending[LEARN_ENDING_LEN];
    memcpy(ending, line + start, len);
    ending[len] = '\0';

    int empty = -1;
    for (int i = 0; i < LEARN_TABLE; i++) {
        if (l->habit[i].count == 0) { if (empty < 0) empty = i; continue; }
        if (strcmp(l->habit[i].ending, ending) == 0) { l->habit[i].count++; return; }
    }
    if (empty < 0) {                        /* table full: forget the rarest */
        empty = 0;
        for (int i = 1; i < LEARN_TABLE; i++)
            if (l->habit[i].count < l->habit[empty].count) empty = i;
    }
    snprintf(l->habit[empty].ending, LEARN_ENDING_LEN, "%s", ending);
    l->habit[empty].count = 1;
}

const char *learn_habit(const struct learn *l, int *count)
{
    int best = -1;
    for (int i = 0; i < LEARN_TABLE; i++)
        if (l->habit[i].count >= 2 && (best < 0 || l->habit[i].count > l->habit[best].count)) best = i;
    if (best < 0) return NULL;
    if (count) *count = l->habit[best].count;
    return l->habit[best].ending;
}

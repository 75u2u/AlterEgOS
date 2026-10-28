#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "ime.h"
#include "../agent/utf8.h"

void ime_init(struct ime *m, const struct dict *d)
{
    memset(m, 0, sizeof *m);
    m->dict = d;
    kana_reset(&m->kana);
}

int ime_active(const struct ime *m) { return m->pre[0] || m->kana.n > 0 || m->converting; }

static void pre_append(struct ime *m, const char *s)
{
    size_t n = strlen(m->pre), k = strlen(s);
    if (n + k >= sizeof m->pre) return;
    memcpy(m->pre + n, s, k + 1);
}

/* Move the romaji still pending into the preedit. */
static void settle(struct ime *m)
{
    char out[32];
    size_t n = kana_flush(&m->kana, out, sizeof out);
    out[n] = '\0';
    if (n) pre_append(m, out);
}

static size_t put(char *out, size_t o, size_t cap, const char *s)
{
    size_t k = strlen(s);
    if (o + k >= cap) return o;
    memcpy(out + o, s, k);
    out[o + k] = '\0';
    return o + k;
}

/* Commit the shown candidate; the rest of the preedit stays. */
static size_t commit_candidate(struct ime *m, char *out, size_t o, size_t cap)
{
    o = put(out, o, cap, m->cands[m->cur]);
    memmove(m->pre, m->pre + m->seg, strlen(m->pre + m->seg) + 1);
    m->converting = 0;
    m->ncands = 0;
    return o;
}

static size_t commit_all(struct ime *m, char *out, size_t o, size_t cap)
{
    if (m->converting) o = commit_candidate(m, out, o, cap);
    settle(m);
    o = put(out, o, cap, m->pre);
    m->pre[0] = '\0';
    return o;
}

static void convert(struct ime *m)
{
    settle(m);
    if (!m->pre[0]) return;
    struct dict none = {0};
    m->ncands = dict_convert(m->dict ? m->dict : &none, m->pre, m->cands, IME_CANDS, &m->seg);
    m->cur = 0;
    m->converting = m->ncands > 0;
}

static void pre_backspace(struct ime *m)
{
    size_t n = strlen(m->pre);
    while (n > 0 && ((unsigned char)m->pre[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    m->pre[n] = '\0';
}

size_t ime_key(struct ime *m, const char *text, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    unsigned char c = (unsigned char)text[0];
    int single = text[1] == '\0';

    if (single && c == 0x7F) {                                   /* Backspace */
        if (m->converting) { m->converting = 0; m->ncands = 0; return 0; }
        if (kana_backspace(&m->kana)) return 0;
        if (m->pre[0]) { pre_backspace(m); return 0; }
        return put(out, 0, cap, text);
    }
    if (single && c == '\r') {                                    /* Enter */
        if (m->converting) return commit_candidate(m, out, 0, cap);   /* the candidate only: nothing is sent yet */
        if (ime_active(m)) { settle(m); o = put(out, 0, cap, m->pre); m->pre[0] = '\0'; }
        return put(out, o, cap, text);                                /* plain kana: commit and send the line */
    }
    if (single && c == 0x1B) {                                    /* Escape */
        if (m->converting) { m->converting = 0; m->ncands = 0; return 0; }
        if (ime_active(m)) { kana_reset(&m->kana); m->pre[0] = '\0'; return 0; }
        return put(out, 0, cap, text);
    }
    if (single && c == ' ') {                                     /* Space: convert / next */
        if (m->converting) { m->cur = (m->cur + 1) % m->ncands; return 0; }
        settle(m);
        if (!m->pre[0]) return put(out, 0, cap, text);
        convert(m);
        return 0;
    }
    if (single && m->converting && c >= '1' && c <= '9') {         /* pick by number */
        int i = c - '1';
        if (i < m->ncands) { m->cur = i; return commit_candidate(m, out, 0, cap); }
        return 0;
    }
    if (single && c >= 0x20 && c < 0x7F) {                         /* a letter: more preedit */
        if (m->converting) o = commit_candidate(m, out, o, cap);
        char kana[64];
        size_t n = kana_feed(&m->kana, c, kana, sizeof kana);
        kana[n] = '\0';
        if (n) pre_append(m, kana);
        return o;
    }
    /* anything else (arrows, control): commit and pass it on */
    o = commit_all(m, out, o, cap);
    return put(out, o, cap, text);
}

size_t ime_flush(struct ime *m, char *out, size_t cap)
{
    out[0] = '\0';
    return commit_all(m, out, 0, cap);
}

void ime_shown(const struct ime *m, char *buf, size_t cap, size_t *conv)
{
    buf[0] = '\0';
    *conv = 0;
    if (m->converting) {
        snprintf(buf, cap, "%s%s", m->cands[m->cur], m->pre + m->seg);
        *conv = strlen(m->cands[m->cur]);
    } else {
        snprintf(buf, cap, "%s%s", m->pre, m->kana.pend);
    }
}

void ime_popup(const struct ime *m, char *buf, size_t cap, int *cur_index)
{
    buf[0] = '\0';
    *cur_index = m->cur;
    if (!m->converting) return;
    size_t o = 0;
    int first = (m->cur / 9) * 9;
    for (int i = first; i < m->ncands && i < first + 9; i++) {
        char item[DICT_CAND + 8];
        snprintf(item, sizeof item, "%s%d %s", i > first ? "  " : "", i - first + 1, m->cands[i]);
        o = put(buf, o, cap, item);
    }
    if (m->ncands > first + 9 || first > 0) o = put(buf, o, cap, "  …");
}

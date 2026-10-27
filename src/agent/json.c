#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

void sbuf_init(struct sbuf *b) { b->p = NULL; b->len = b->cap = 0; }
void sbuf_free(struct sbuf *b) { free(b->p); sbuf_init(b); }

static int sbuf_reserve(struct sbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

int sbuf_add(struct sbuf *b, const char *s, size_t n)
{
    if (sbuf_reserve(b, n) < 0) return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

int sbuf_adds(struct sbuf *b, const char *s) { return sbuf_add(b, s, strlen(s)); }

int sbuf_addf(struct sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    int n = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    int r = sbuf_add(b, s, (size_t)n);
    free(s);
    return r;
}

int sbuf_add_json_string(struct sbuf *b, const char *s)
{
    if (sbuf_add(b, "\"", 1) < 0) return -1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        const char *esc = NULL;
        char u[8];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) { snprintf(u, sizeof u, "\\u%04x", c); esc = u; }
        }
        if (esc ? sbuf_adds(b, esc) < 0 : sbuf_add(b, (const char *)&c, 1) < 0) return -1;
    }
    return sbuf_add(b, "\"", 1);
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static void put_utf8(struct sbuf *b, unsigned cp)
{
    char u[4];
    size_t n;
    if (cp < 0x80)          { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800)    { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000)  { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else                    { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    sbuf_add(b, u, n);
}

char *json_unquote(const char *src, const char **endp)
{
    struct sbuf b;
    sbuf_init(&b);
    const char *s = src;
    while (*s && *s != '"') {
        if (*s != '\\') { sbuf_add(&b, s, 1); s++; continue; }
        s++;
        switch (*s) {
        case '"':  sbuf_add(&b, "\"", 1); s++; break;
        case '\\': sbuf_add(&b, "\\", 1); s++; break;
        case '/':  sbuf_add(&b, "/", 1);  s++; break;
        case 'b':  sbuf_add(&b, "\b", 1); s++; break;
        case 'f':  sbuf_add(&b, "\f", 1); s++; break;
        case 'n':  sbuf_add(&b, "\n", 1); s++; break;
        case 'r':  sbuf_add(&b, "\r", 1); s++; break;
        case 't':  sbuf_add(&b, "\t", 1); s++; break;
        case 'u': {
            unsigned cp;
            if (hex4(s + 1, &cp) < 0) goto bad;
            s += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF && s[0] == '\\' && s[1] == 'u') {
                unsigned lo;
                if (hex4(s + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s += 6;
                }
            }
            put_utf8(&b, cp);
            break;
        }
        default: goto bad;
        }
    }
    if (*s != '"') goto bad;
    if (endp) *endp = s;
    if (!b.p) return strdup("");
    return b.p;
bad:
    sbuf_free(&b);
    return NULL;
}

const char *json_value(const char *json, const char *key)
{
    char pat[80];
    int n = snprintf(pat, sizeof pat, "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof pat) return NULL;
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += n;
    while (*p == ' ') p++;
    return p;
}

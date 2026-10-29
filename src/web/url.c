/* URLs: parsing, resolution of relative references, query encoding. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "web.h"

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

int url_parse(const char *s, struct url *u)
{
    memset(u, 0, sizeof *u);
    const char *p = strstr(s, ":");
    if (!p || p == s) return -1;
    for (const char *q = s; q < p; q++)
        if (!isalnum((unsigned char)*q) && *q != '+' && *q != '-' && *q != '.') return -1;
    snprintf(u->scheme, sizeof u->scheme, "%.*s", (int)(p - s), s);
    lower(u->scheme);
    p++;
    if (strcmp(u->scheme, "about") == 0) {
        snprintf(u->path, sizeof u->path, "%s", p);
        return 0;
    }
    if (strncmp(p, "//", 2) != 0) {
        if (strcmp(u->scheme, "file") == 0) { snprintf(u->path, sizeof u->path, "%s", p); return 0; }
        return -1;
    }
    p += 2;
    const char *end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) p = at + 1;                                   /* user:pass@ is ignored */
    const char *colon = NULL;
    if (*p == '[') {                                      /* [v6]:port */
        const char *close = memchr(p, ']', (size_t)(end - p));
        if (!close) return -1;
        snprintf(u->host, sizeof u->host, "%.*s", (int)(close - p - 1), p + 1);
        if (close + 1 < end && close[1] == ':') colon = close + 1;
    } else {
        colon = memchr(p, ':', (size_t)(end - p));
        snprintf(u->host, sizeof u->host, "%.*s", (int)((colon ? colon : end) - p), p);
    }
    lower(u->host);
    if (colon) u->port = atoi(colon + 1);
    if (strcmp(u->scheme, "file") != 0 && u->host[0] == '\0') return -1;
    const char *frag = strchr(end, '#');
    size_t plen = frag ? (size_t)(frag - end) : strlen(end);
    if (plen == 0 || *end == '?') snprintf(u->path, sizeof u->path, "/%.*s", (int)plen, end);
    else snprintf(u->path, sizeof u->path, "%.*s", (int)plen, end);
    return 0;
}

/* Remove "." and ".." segments in place (RFC 3986 5.2.4). */
static void remove_dots(char *path)
{
    char out[4096];
    size_t o = 0;
    const char *p = path;
    out[0] = '\0';
    while (*p) {
        if (strncmp(p, "../", 3) == 0) { p += 3; continue; }
        if (strncmp(p, "./", 2) == 0) { p += 2; continue; }
        if (strncmp(p, "/./", 3) == 0) { p += 2; continue; }
        if (strcmp(p, "/.") == 0) { p += 1; if (o + 1 < sizeof out) out[o++] = '/'; continue; }
        if (strncmp(p, "/../", 4) == 0 || strcmp(p, "/..") == 0) {
            p += strcmp(p, "/..") == 0 ? 2 : 3;
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            if (strcmp(p, ".") == 0) { p++; if (o + 1 < sizeof out) out[o++] = '/'; }
            continue;
        }
        if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) break;
        size_t seg = (*p == '/') ? 1 + strcspn(p + 1, "/") : strcspn(p, "/");
        if (o + seg >= sizeof out) break;
        memcpy(out + o, p, seg);
        o += seg;
        p += seg;
    }
    out[o] = '\0';
    strcpy(path, out);
}

static size_t put_authority(const struct url *u, char *out, size_t cap)
{
    if (u->port) return (size_t)snprintf(out, cap, "%s://%s:%d", u->scheme, u->host, u->port);
    return (size_t)snprintf(out, cap, "%s://%s", u->scheme, u->host);
}

size_t url_resolve(const char *base, const char *ref, char *out, size_t cap)
{
    struct url b, r;
    size_t n;
    char tmp[4096];
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r') ref++;
    snprintf(tmp, sizeof tmp, "%s", ref);
    char *h = strchr(tmp, '#');
    if (h) *h = '\0';
    ref = tmp;
    if (url_parse(ref, &r) == 0) {                        /* already absolute */
        if (strcmp(r.scheme, "about") == 0 || strcmp(r.scheme, "file") == 0) return (size_t)snprintf(out, cap, "%s:%s", r.scheme, r.path);
        n = put_authority(&r, out, cap);
        if (n < cap) n += (size_t)snprintf(out + n, cap - n, "%s", r.path);
        return n < cap ? n : cap - 1;
    }
    if (url_parse(base, &b) < 0) return (size_t)snprintf(out, cap, "%s", ref);
    if (ref[0] == '\0') {                                 /* "" and "#frag": the page itself */
        if (strcmp(b.scheme, "about") == 0 || strcmp(b.scheme, "file") == 0) return (size_t)snprintf(out, cap, "%s:%s", b.scheme, b.path);
        n = put_authority(&b, out, cap);
        if (n < cap) n += (size_t)snprintf(out + n, cap - n, "%s", b.path);
        return n < cap ? n : cap - 1;
    }
    if (strcmp(b.scheme, "file") == 0 || strcmp(b.scheme, "about") == 0) b.port = 0;
    if (strncmp(ref, "//", 2) == 0) return (size_t)snprintf(out, cap, "%s:%s", b.scheme, ref);
    char path[4096];
    if (ref[0] == '/') snprintf(path, sizeof path, "%s", ref);
    else if (ref[0] == '?' || ref[0] == '\0') {
        size_t pl = strcspn(b.path, "?");
        snprintf(path, sizeof path, "%.*s%s", (int)pl, b.path, ref);
    } else {
        size_t pl = strcspn(b.path, "?");
        while (pl > 0 && b.path[pl - 1] != '/') pl--;
        snprintf(path, sizeof path, "%.*s%s", (int)pl, b.path, ref);
    }
    remove_dots(path);
    if (strcmp(b.scheme, "file") == 0) return (size_t)snprintf(out, cap, "file:%s", path);
    n = put_authority(&b, out, cap);
    if (n < cap) n += (size_t)snprintf(out + n, cap - n, "%s", path);
    return n < cap ? n : cap - 1;
}

size_t url_encode(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
    }
    out[o] = '\0';
    return o;
}

size_t url_unescape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            s += 2;
        } else if (*s == '+') out[o++] = ' ';
        else out[o++] = *s;
    }
    out[o] = '\0';
    return o;
}

/* HTML to text. No DOM: one pass over the tags, keeping only what a text
 * screen can show. Block elements start lines, paragraphs and headings
 * are separated by a blank line, list items get a bullet or a number,
 * links are underlined and numbered [n], images show their alt text.
 * Scripts, styles and the like are dropped. Then the layout wraps the
 * text to the terminal width, two cells per CJK character. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../agent/utf8.h"
#include "web.h"

/* ---- the output buffer */

static int put(struct doc *d, const char *s, size_t n)
{
    if (d->len + n + 1 > d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 8192;
        while (cap < d->len + n + 1) cap *= 2;
        char *np = realloc(d->text, cap);
        if (!np) return -1;
        d->text = np;
        d->cap = cap;
    }
    memcpy(d->text + d->len, s, n);
    d->len += n;
    d->text[d->len] = '\0';
    return 0;
}

static int putc_(struct doc *d, char c) { return put(d, &c, 1); }

void doc_free(struct doc *d)
{
    free(d->text);
    for (int i = 0; i < d->nlinks; i++) free(d->links[i]);
    free(d->links);
    memset(d, 0, sizeof *d);
}

static int add_link(struct doc *d, const char *url)
{
    if (d->nlinks == d->lcap) {
        int cap = d->lcap ? d->lcap * 2 : 64;
        char **nl = realloc(d->links, (size_t)cap * sizeof *nl);
        if (!nl) return -1;
        d->links = nl;
        d->lcap = cap;
    }
    d->links[d->nlinks] = strdup(url);
    if (!d->links[d->nlinks]) return -1;
    return ++d->nlinks;
}

/* ---- entities */

static const struct { const char *name; uint32_t cp; } g_entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "laquo", 0xAB }, { "raquo", 0xBB }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "bull", 0x2022 }, { "middot", 0xB7 }, { "times", 0xD7 }, { "divide", 0xF7 }, { "yen", 0xA5 }, { "euro", 0x20AC },
    { "pound", 0xA3 }, { "cent", 0xA2 }, { "deg", 0xB0 }, { "plusmn", 0xB1 }, { "para", 0xB6 }, { "sect", 0xA7 },
    { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "hearts", 0x2665 }, { "ensp", 0x2002 },
    { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwnj", 0 }, { "zwj", 0 }, { "shy", 0 }, { "iexcl", 0xA1 }, { "iquest", 0xBF },
    { "frac12", 0xBD }, { "frac14", 0xBC }, { "sup2", 0xB2 }, { "micro", 0xB5 }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
    { "agrave", 0xE0 }, { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 }, { "ccedil", 0xE7 }, { "ntilde", 0xF1 },
};

/* Decode an entity at `s` ('&' included). Writes the code point, returns the bytes consumed (0 if not an entity). */
static size_t entity(const char *s, size_t n, uint32_t *cp)
{
    if (n < 3 || s[0] != '&') return 0;
    size_t i = 1;
    if (s[1] == '#') {
        i = 2;
        int hex = (i < n && (s[i] == 'x' || s[i] == 'X'));
        if (hex) i++;
        size_t start = i;
        uint32_t v = 0;
        while (i < n && i < start + 8 && (hex ? isxdigit((unsigned char)s[i]) : isdigit((unsigned char)s[i]))) {
            v = hex ? v * 16 + (uint32_t)(isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10)) : v * 10 + (uint32_t)(s[i] - '0');
            i++;
        }
        if (i == start) return 0;
        if (i < n && s[i] == ';') i++;
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
        if (v >= 0x80 && v <= 0x9F) {             /* Windows-1252 leftovers */
            static const uint16_t w[32] = { 0x20AC, 0, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17D, 0,
                                            0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0, 0x17E, 0x178 };
            if (w[v - 0x80]) v = w[v - 0x80];
        }
        *cp = v;
        return i;
    }
    while (i < n && i < 12 && isalnum((unsigned char)s[i])) i++;
    if (i == 1) return 0;
    for (size_t k = 0; k < sizeof g_entities / sizeof g_entities[0]; k++) {
        size_t nl = strlen(g_entities[k].name);
        if (nl == i - 1 && strncmp(s + 1, g_entities[k].name, nl) == 0) {
            *cp = g_entities[k].cp;
            if (i < n && s[i] == ';') i++;
            return i;
        }
    }
    return 0;
}

/* ---- the renderer state */

struct render {
    struct doc *d;
    const char *base;
    int at_line_start;          /* nothing on the current line yet */
    int blank_lines;            /* blank lines already emitted after the last text */
    int pending_space;          /* collapsed whitespace waiting for the next word */
    int pre;                    /* inside <pre>: keep whitespace */
    int skip;                   /* inside script/style/...: drop text; counts nesting of the same tag */
    char skip_tag[16];
    int in_title;
    int in_link;
    int bold, head;
    int list_depth;
    int ol_count[16];           /* -1 for <ul>, else the next number */
    int td;                     /* cells already in the current table row */
    char link_url[4096];
    size_t link_text_start;
    int head_pending_newline;
};

static void text_cp(struct render *r, uint32_t cp);

static void newline(struct render *r)
{
    if (r->at_line_start) return;
    putc_(r->d, '\n');
    r->at_line_start = 1;
    r->pending_space = 0;
    r->td = 0;
}

/* End the line (if any) and leave one blank line. */
static void paragraph(struct render *r)
{
    if (!r->at_line_start) newline(r);
    if (r->blank_lines == 0 && r->d->len > 0) { putc_(r->d, '\n'); r->blank_lines = 1; }
}

static void text_cp(struct render *r, uint32_t cp)
{
    char u[4];
    if (r->in_title) {
        if (cp == '\n' || cp == '\t' || cp == '\r' || cp == 0xA0) cp = ' ';
        size_t tl = strlen(r->d->title);
        if (cp == ' ' && (tl == 0 || r->d->title[tl - 1] == ' ')) return;
        size_t m = utf8_encode(cp, u);
        if (tl + m < sizeof r->d->title) { memcpy(r->d->title + tl, u, m); r->d->title[tl + m] = '\0'; }
        return;
    }
    if (r->skip) return;
    if (r->pre) {
        if (cp == '\r') return;
        if (cp == '\n') { newline(r); return; }
        if (cp == 0xA0) cp = ' ';
        if (cp == '\t') { put(r->d, "    ", 4); r->at_line_start = 0; r->blank_lines = 0; return; }
    } else {
        if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r' || cp == '\f') { r->pending_space = 1; return; }
        if (cp == 0xA0) cp = ' ';
        if (r->pending_space && !r->at_line_start) putc_(r->d, ' ');
        r->pending_space = 0;
    }
    if (cp == 0) return;
    size_t m = utf8_encode(cp, u);
    put(r->d, u, m);
    r->at_line_start = 0;
    r->blank_lines = 0;
}

static void text(struct render *r, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        size_t m = entity(s + i, n - i, &cp);
        if (!m) m = utf8_decode(s + i, &cp);
        if (!m) break;
        text_cp(r, cp);
        i += m;
    }
}

static void text_str(struct render *r, const char *s) { text(r, s, strlen(s)); }

/* ---- tags */

struct tag {
    char name[16];
    int closing;
    const char *attrs;          /* inside the tag, after the name */
    size_t alen;
};

/* Value of an attribute (entities decoded) or 0 when absent. */
static int attr(const struct tag *t, const char *name, char *out, size_t cap)
{
    const char *p = t->attrs, *end = t->attrs + t->alen;
    size_t nl = strlen(name);
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == '/')) p++;
        const char *ns = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/') p++;
        size_t klen = (size_t)(p - ns);
        if (klen == 0) { p++; continue; }
        while (p < end && isspace((unsigned char)*p)) p++;
        const char *vs = NULL, *ve = NULL;
        if (p < end && *p == '=') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                vs = p;
                while (p < end && *p != q) p++;
                ve = p;
                if (p < end) p++;
            } else {
                vs = p;
                while (p < end && !isspace((unsigned char)*p)) p++;
                ve = p;
            }
        }
        if (klen == nl && strncasecmp(ns, name, nl) == 0) {
            size_t o = 0;
            if (vs) {
                const char *s = vs;
                while (s < ve && o + 4 < cap) {
                    uint32_t cp;
                    size_t m = entity(s, (size_t)(ve - s), &cp);
                    if (!m) m = utf8_decode(s, &cp);
                    if (!m) break;
                    char u[4];
                    size_t k = utf8_encode(cp, u);
                    memcpy(out + o, u, k);
                    o += k;
                    s += m;
                }
            }
            out[o] = '\0';
            return 1;
        }
    }
    out[0] = '\0';
    return 0;
}

static int is(const struct tag *t, const char *n) { return strcmp(t->name, n) == 0; }

static int block_tag(const struct tag *t)
{
    static const char *blocks[] = { "address", "article", "aside", "blockquote", "center", "dd", "details", "dialog", "dir", "div", "dl", "dt",
        "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "li", "main", "menu", "nav",
        "ol", "p", "pre", "section", "summary", "table", "tbody", "thead", "tfoot", "tr", "ul", "caption", "legend", "option", "body", "html", "noscript", NULL };
    for (int i = 0; blocks[i]; i++) if (is(t, blocks[i])) return 1;
    return 0;
}

static int para_tag(const struct tag *t)
{
    return is(t, "p") || is(t, "h1") || is(t, "h2") || is(t, "h3") || is(t, "h4") || is(t, "h5") || is(t, "h6") || is(t, "blockquote") ||
           is(t, "pre") || is(t, "table") || is(t, "hr") || is(t, "figure") || is(t, "article") || is(t, "section") || is(t, "header") ||
           is(t, "footer") || is(t, "nav") || is(t, "main") || is(t, "aside") || is(t, "dl") || is(t, "form") || is(t, "fieldset");
}

static void end_link(struct render *r)
{
    if (!r->in_link) return;
    r->in_link = 0;
    int n = add_link(r->d, r->link_url);
    if (r->d->len == r->link_text_start) text_str(r, "リンク");            /* an image-only link still needs a label */
    char tag[16];
    snprintf(tag, sizeof tag, "[%d]", n);
    put(r->d, tag, strlen(tag));
    putc_(r->d, TX_LINK_OFF);
}

static void handle_tag(struct render *r, const struct tag *t)
{
    char v[4096];
    if (r->skip) {
        if (t->closing && is(t, r->skip_tag)) r->skip--;
        else if (!t->closing && is(t, r->skip_tag)) r->skip++;
        return;
    }
    if (r->in_title) {
        if (t->closing && is(t, "title")) {
            r->in_title = 0;
            size_t tl = strlen(r->d->title);
            while (tl > 0 && r->d->title[tl - 1] == ' ') r->d->title[--tl] = '\0';
        }
        return;
    }
    if (!t->closing && (is(t, "script") || is(t, "style") || is(t, "template") || is(t, "svg") || is(t, "head") || is(t, "iframe") || is(t, "object") || is(t, "noscript"))) {
        if (is(t, "head")) return;                                              /* <title> and <meta> are inside */
        snprintf(r->skip_tag, sizeof r->skip_tag, "%s", t->name);
        r->skip = 1;
        return;
    }
    if (is(t, "title")) {
        if (!t->closing) { r->in_title = 1; r->d->title[0] = '\0'; }
        return;
    }
    if (is(t, "meta") && !t->closing) {
        if (attr(t, "charset", v, sizeof v)) snprintf(r->d->charset, sizeof r->d->charset, "%s", v);
        else if (attr(t, "content", v, sizeof v)) {
            const char *cs = strcasestr(v, "charset=");
            if (cs) snprintf(r->d->charset, sizeof r->d->charset, "%.*s", (int)strcspn(cs + 8, " ;\"'"), cs + 8);
        }
        for (char *p = r->d->charset; *p; p++) *p = (char)tolower((unsigned char)*p);
        return;
    }
    if (is(t, "br")) { if (r->at_line_start) { putc_(r->d, '\n'); r->blank_lines++; } else newline(r); return; }
    if (is(t, "a")) {
        if (t->closing) end_link(r);
        else {
            end_link(r);
            if (attr(t, "href", v, sizeof v) && v[0] && v[0] != '#' && strncasecmp(v, "javascript:", 11) != 0 && strncasecmp(v, "mailto:", 7) != 0 && strncasecmp(v, "tel:", 4) != 0) {
                url_resolve(r->base, v, r->link_url, sizeof r->link_url);
                r->in_link = 1;
                if (r->pending_space && !r->at_line_start) { putc_(r->d, ' '); r->pending_space = 0; }
                putc_(r->d, TX_LINK_ON);
                r->link_text_start = r->d->len;
            }
        }
        return;
    }
    if (is(t, "img") && !t->closing) {
        if (attr(t, "alt", v, sizeof v) && v[0]) { text_str(r, "[画像: "); text_str(r, v); text_str(r, "]"); }
        else if (r->in_link) text_str(r, "[画像]");
        return;
    }
    if (is(t, "input") && !t->closing) {
        char ty[32];
        attr(t, "type", ty, sizeof ty);
        if (strcasecmp(ty, "hidden") == 0) return;
        if (strcasecmp(ty, "submit") == 0 || strcasecmp(ty, "button") == 0) {
            if (!attr(t, "value", v, sizeof v) || !v[0]) snprintf(v, sizeof v, "送信");
            text_str(r, "[ "); text_str(r, v); text_str(r, " ]");
        } else if (strcasecmp(ty, "checkbox") == 0 || strcasecmp(ty, "radio") == 0) text_str(r, "[ ]");
        else if (strcasecmp(ty, "image") != 0) {
            attr(t, "placeholder", v, sizeof v);
            text_str(r, "[入力欄"); if (v[0]) { text_str(r, ": "); text_str(r, v); } text_str(r, "]");
        }
        return;
    }
    if (is(t, "button")) { text_str(r, t->closing ? " ]" : "[ "); return; }
    if (is(t, "textarea")) { if (!t->closing) text_str(r, "[入力欄]"); snprintf(r->skip_tag, sizeof r->skip_tag, "textarea"); if (!t->closing) r->skip = 1; return; }
    if (is(t, "select")) { if (!t->closing) text_str(r, "[選択]"); snprintf(r->skip_tag, sizeof r->skip_tag, "select"); if (!t->closing) r->skip = 1; return; }
    if (is(t, "b") || is(t, "strong") || is(t, "th") || is(t, "dt")) {
        if (is(t, "th") || is(t, "dt")) { if (!t->closing && is(t, "dt")) newline(r); if (!t->closing && is(t, "th") && r->td++) { put(r->d, "  ", 2); r->pending_space = 0; } }
        if (!t->closing && !r->bold) { putc_(r->d, TX_BOLD_ON); r->bold = 1; }
        else if (t->closing && r->bold) { putc_(r->d, TX_BOLD_OFF); r->bold = 0; }
        return;
    }
    if (is(t, "td")) { if (!t->closing && r->td++) { put(r->d, "  ", 2); r->pending_space = 0; } return; }
    if (is(t, "pre")) { paragraph(r); r->pre = !t->closing; return; }
    if (is(t, "hr")) { paragraph(r); text_str(r, "----------------------------------------"); paragraph(r); return; }
    if (t->name[0] == 'h' && t->name[1] >= '1' && t->name[1] <= '6' && t->name[2] == '\0') {
        paragraph(r);
        if (!t->closing) { putc_(r->d, TX_HEAD_ON); r->head = 1; }
        else if (r->head) { putc_(r->d, TX_HEAD_OFF); r->head = 0; }
        return;
    }
    if (is(t, "ul") || is(t, "ol")) {
        if (!t->closing) {
            if (r->list_depth == 0) paragraph(r); else newline(r);
            if (r->list_depth < 16) r->ol_count[r->list_depth] = is(t, "ol") ? 1 : -1;
            r->list_depth++;
        } else if (r->list_depth > 0) {
            r->list_depth--;
            if (r->list_depth == 0) paragraph(r); else newline(r);
        }
        return;
    }
    if (is(t, "li")) {
        if (t->closing) { newline(r); return; }
        newline(r);
        int depth = r->list_depth > 0 ? r->list_depth - 1 : 0;
        if (depth > 15) depth = 15;
        for (int i = 0; i < depth; i++) put(r->d, "  ", 2);
        if (r->list_depth > 0 && r->ol_count[depth] > 0) { snprintf(v, sizeof v, "%d. ", r->ol_count[depth]++); put(r->d, v, strlen(v)); }
        else put(r->d, "・", 3);
        r->at_line_start = 0;
        r->blank_lines = 0;
        r->pending_space = 0;
        return;
    }
    if (is(t, "dd")) { if (!t->closing) { newline(r); put(r->d, "    ", 4); r->at_line_start = 0; r->blank_lines = 0; r->pending_space = 0; } return; }
    if (is(t, "blockquote")) {
        paragraph(r);
        return;
    }
    if (block_tag(t)) {
        if (para_tag(t)) paragraph(r); else newline(r);
        return;
    }
    /* inline tags (span, em, i, u, code, small, label, ...): nothing to do */
}

/* ---- the parser */

static int is_raw(const char *name)
{
    return !strcmp(name, "script") || !strcmp(name, "style") || !strcmp(name, "textarea") || !strcmp(name, "template") ||
           !strcmp(name, "iframe") || !strcmp(name, "noscript") || !strcmp(name, "svg") || !strcmp(name, "object") || !strcmp(name, "select");
}

/* Case-insensitive search in a buffer that is not NUL-terminated. */
static const char *strcasestr_n(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (strncasecmp(s + i, needle, m) == 0) return s + i;
    return NULL;
}

static int looks_like_html(const char *s, size_t n, const char *ctype)
{
    if (ctype && (strstr(ctype, "text/html") || strstr(ctype, "application/xhtml"))) return 1;
    if (ctype && (strncmp(ctype, "text/", 5) == 0 || strstr(ctype, "json") || strstr(ctype, "xml"))) return 0;
    size_t i = 0;
    while (i < n && isspace((unsigned char)s[i])) i++;
    return i < n && s[i] == '<';
}

int text_to_doc(const char *text_, size_t n, struct doc *d)
{
    memset(d, 0, sizeof *d);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text_[i];
        if (c == '\r') continue;
        if (c == '\t') { if (put(d, "    ", 4) < 0) return -1; continue; }
        if (c < 0x20 && c != '\n') continue;
        if (putc_(d, (char)c) < 0) return -1;
    }
    if (!d->text && put(d, "", 0) < 0) return -1;
    return 0;
}

int html_to_text(const char *html, size_t n, const char *base, const char *ctype, struct doc *d)
{
    if (!looks_like_html(html, n, ctype)) return text_to_doc(html, n, d);
    memset(d, 0, sizeof *d);
    if (put(d, "", 0) < 0) return -1;
    struct render r;
    memset(&r, 0, sizeof r);
    r.d = d;
    r.base = base;
    r.at_line_start = 1;
    r.blank_lines = 1;
    size_t i = 0;
    while (i < n) {
        if (r.skip && !r.in_title && (is_raw(r.skip_tag))) {      /* script, style, ...: skip to </name */
            char close[24];
            snprintf(close, sizeof close, "</%s", r.skip_tag);
            const char *e = strcasestr_n(html + i, n - i, close);
            if (!e) break;
            i = (size_t)(e - html);
            r.skip = 0;
            /* fall through to parse the closing tag, which handle_tag then ignores */
            size_t j = i + 2 + strlen(r.skip_tag);
            while (j < n && html[j] != '>') j++;
            i = j < n ? j + 1 : n;
            continue;
        }
        if (html[i] != '<') {
            size_t j = i;
            while (j < n && html[j] != '<') j++;
            if (r.skip && !r.in_title) { i = j; continue; }
            text(&r, html + i, j - i);
            i = j;
            continue;
        }
        if (i + 4 <= n && strncmp(html + i, "<!--", 4) == 0) {
            const char *e = memmem(html + i + 4, n - i - 4, "-->", 3);
            i = e ? (size_t)(e - html) + 3 : n;
            continue;
        }
        if (i + 2 <= n && (html[i + 1] == '!' || html[i + 1] == '?')) {        /* doctype, processing instruction */
            const char *e = memchr(html + i, '>', n - i);
            i = e ? (size_t)(e - html) + 1 : n;
            continue;
        }
        size_t j = i + 1;
        struct tag t;
        memset(&t, 0, sizeof t);
        if (j < n && html[j] == '/') { t.closing = 1; j++; }
        size_t ns = j;
        while (j < n && (isalnum((unsigned char)html[j]) || html[j] == '-' || html[j] == ':')) j++;
        if (j == ns) {                                                         /* a lone '<': text */
            text(&r, html + i, 1);
            i++;
            continue;
        }
        snprintf(t.name, sizeof t.name, "%.*s", (int)(j - ns < sizeof t.name - 1 ? j - ns : sizeof t.name - 1), html + ns);
        for (char *p = t.name; *p; p++) *p = (char)tolower((unsigned char)*p);
        /* the end of the tag: '>' outside quotes */
        size_t k = j;
        char q = 0;
        while (k < n) {
            if (q) { if (html[k] == q) q = 0; }
            else if (html[k] == '"' || html[k] == '\'') q = html[k];
            else if (html[k] == '>') break;
            k++;
        }
        t.attrs = html + j;
        t.alen = k - j;
        handle_tag(&r, &t);
        i = k < n ? k + 1 : n;
    }
    end_link(&r);
    if (r.bold) putc_(d, TX_BOLD_OFF);
    if (r.head) putc_(d, TX_HEAD_OFF);
    if (!r.at_line_start) putc_(d, '\n');
    /* trim blank lines at both ends (one '\n' closes the last line) */
    size_t lead = 0;
    while (lead < d->len && d->text[lead] == '\n') lead++;
    if (lead) { memmove(d->text, d->text + lead, d->len - lead + 1); d->len -= lead; }
    while (d->len > 1 && d->text[d->len - 1] == '\n' && d->text[d->len - 2] == '\n') d->text[--d->len] = '\0';
    return 0;
}

void doc_strip(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 1 < cap; in++)
        if ((unsigned char)*in >= 0x20 || *in == '\n' || *in == '\t') out[o++] = *in;
    out[o] = '\0';
}

/* ---- layout */

void layout_free(struct layout *l)
{
    free(l->lines);
    memset(l, 0, sizeof *l);
}

static int add_line(struct layout *l, size_t start, size_t end, uint8_t attr)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        struct line *nl = realloc(l->lines, (size_t)cap * sizeof *nl);
        if (!nl) return -1;
        l->lines = nl;
        l->cap = cap;
    }
    l->lines[l->n].start = start;
    l->lines[l->n].end = end;
    l->lines[l->n].attr = attr;
    l->n++;
    return 0;
}

#define LA_BOLD 1
#define LA_LINK 2
#define LA_HEAD 4

static uint8_t apply_ctl(uint8_t attr, unsigned char c)
{
    switch (c) {
    case TX_BOLD_ON: return attr | LA_BOLD;
    case TX_BOLD_OFF: return attr & ~LA_BOLD;
    case TX_LINK_ON: return attr | LA_LINK;
    case TX_LINK_OFF: return attr & ~LA_LINK;
    case TX_HEAD_ON: return attr | LA_HEAD;
    case TX_HEAD_OFF: return attr & ~LA_HEAD;
    default: return attr;
    }
}

int layout_doc(const struct doc *d, int width, struct layout *l)
{
    memset(l, 0, sizeof *l);
    if (width < 8) width = 8;
    const char *s = d->text;
    size_t i = 0, line_start = 0, brk = 0;         /* brk: position after the last place a break is allowed */
    int col = 0, brk_col = 0;
    uint8_t attr = 0, line_attr = 0, brk_attr = 0;
    while (i < d->len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') {
            if (add_line(l, line_start, i, line_attr) < 0) return -1;
            i++;
            line_start = i;
            line_attr = attr;
            col = 0;
            brk = 0;
            continue;
        }
        if (c < 0x20) { attr = apply_ctl(attr, c); i++; continue; }
        uint32_t cp;
        size_t m = utf8_decode(s + i, &cp);
        if (!m) break;
        int w = utf8_wide(cp) ? 2 : 1;
        if (col + w > width) {                      /* wrap: at the last space or CJK boundary, else here */
            if (cp == ' ') { brk = i; brk_attr = attr; }
            size_t at = (brk > line_start) ? brk : i;
            uint8_t a = (brk > line_start) ? brk_attr : attr;
            size_t end = at;
            while (end > line_start && s[end - 1] == ' ') end--;
            if (add_line(l, line_start, end, line_attr) < 0) return -1;
            line_start = at;
            while (line_start < d->len && s[line_start] == ' ') line_start++;
            line_attr = a;
            /* recount the columns of what moved to the new line */
            col = 0;
            for (size_t k = line_start; k < i; ) {
                unsigned char ck = (unsigned char)s[k];
                if (ck < 0x20) { k++; continue; }
                uint32_t cq;
                size_t mk = utf8_decode(s + k, &cq);
                if (!mk) break;
                col += utf8_wide(cq) ? 2 : 1;
                k += mk;
            }
            brk = 0;
            if (col + w > width && col > 0) {       /* one word longer than the line: break it */
                if (add_line(l, line_start, i, line_attr) < 0) return -1;
                line_start = i;
                line_attr = attr;
                col = 0;
            }
        }
        if (cp == ' ') { brk = i + 1; brk_col = col + 1; brk_attr = attr; }
        else if (w == 2) { brk = i; brk_col = col; brk_attr = attr; }   /* before a CJK character */
        col += w;
        i += m;
        if (w == 2) { brk = i; brk_col = col; brk_attr = attr; }        /* and after it */
    }
    (void)brk_col;
    if (line_start < d->len || l->n == 0) { if (add_line(l, line_start, d->len, line_attr) < 0) return -1; }
    return 0;
}

static size_t sgr_for(uint8_t attr, char *out, size_t cap)
{
    if (attr & LA_LINK) return (size_t)snprintf(out, cap, "\x1b[0;4;36m");
    if (attr & LA_HEAD) return (size_t)snprintf(out, cap, "\x1b[0;1;33m");
    if (attr & LA_BOLD) return (size_t)snprintf(out, cap, "\x1b[0;1m");
    return (size_t)snprintf(out, cap, "\x1b[0m");
}

size_t layout_line(const struct doc *d, const struct layout *l, int i, int sgr, char *out, size_t cap)
{
    if (i < 0 || i >= l->n || cap == 0) { if (cap) out[0] = '\0'; return 0; }
    const struct line *ln = &l->lines[i];
    uint8_t attr = ln->attr;
    size_t o = 0;
    if (sgr && attr && o + 16 < cap) o += sgr_for(attr, out + o, cap - o);
    for (size_t k = ln->start; k < ln->end && o + 16 < cap; k++) {
        unsigned char c = (unsigned char)d->text[k];
        if (c < 0x20) {
            uint8_t na = apply_ctl(attr, c);
            if (na != attr && sgr) o += sgr_for(na, out + o, cap - o);
            attr = na;
            continue;
        }
        out[o++] = (char)c;
    }
    if (sgr && o + 8 < cap) o += (size_t)snprintf(out + o, cap - o, "\x1b[0m");
    out[o] = '\0';
    return o;
}

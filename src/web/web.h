#ifndef ALTEREGO_WEB_H
#define ALTEREGO_WEB_H

#include <stddef.h>
#include <stdint.h>

/* alterego-web: AlterEgOS's own small web browser. Text only, no JavaScript,
 * no CSS; it fetches a page (HTTP/1.1, HTTPS through mbedTLS), turns the
 * HTML into text with numbered links, and shows it in the terminal.
 *
 * url.c   URL parsing and resolution of relative links
 * net.c   the HTTP client (chunked bodies, redirects, TLS)
 * html.c  HTML to text: a stream of UTF-8 with a few control bytes
 * main.c  the terminal UI, and --dump for scripts and tests */

/* ---- url.c */

struct url {
    char scheme[16];
    char host[256];
    int port;                   /* 0 = default for the scheme */
    char path[4096];            /* path + query, always starting with '/' (empty for file: and about:) */
};

int    url_parse(const char *s, struct url *u);                      /* -1 if there is no scheme or host */
/* Absolute URL of `ref` seen from the page `base` (RFC 3986 reference
 * resolution, without the fragment). Returns the length written. */
size_t url_resolve(const char *base, const char *ref, char *out, size_t cap);
size_t url_encode(const char *s, char *out, size_t cap);             /* percent-encode a query value (space = +) */
size_t url_unescape(const char *s, char *out, size_t cap);            /* %XX and + in a query */

/* ---- net.c */

#define NET_MAX_BODY (16u << 20)

struct resp {
    int status;                 /* HTTP status, 0 on a transport error */
    char url[4096];             /* the final URL after redirects */
    char ctype[128];            /* Content-Type header (lower case) */
    char *body;                 /* NUL-terminated as well */
    size_t len;
    int insecure;               /* HTTPS without a usable CA bundle: certificate not verified */
    char err[256];              /* when the fetch fails */
};

int   net_fetch(const char *url, struct resp *r);                    /* 0 and r->status set, or -1 with r->err */
void  resp_free(struct resp *r);
int   net_has_tls(void);

/* ---- html.c */

/* Control bytes in the text html_to_text produces. Everything else is UTF-8
 * text and '\n'. The UI maps them to SGR; --dump strips them. */
enum { TX_BOLD_ON = 0x01, TX_BOLD_OFF = 0x02, TX_LINK_ON = 0x03, TX_LINK_OFF = 0x04, TX_HEAD_ON = 0x05, TX_HEAD_OFF = 0x06 };

struct doc {
    char *text;
    size_t len, cap;
    char title[256];
    char charset[32];           /* from <meta charset> / http-equiv, lower case; "" if none */
    char **links;               /* absolute URLs; link [n] in the text is links[n - 1] */
    int nlinks, lcap;
};

void  doc_free(struct doc *d);
/* Render `html` (n bytes, UTF-8) into d->text. `base` is the page's URL, for
 * relative links. `ctype` is the Content-Type header or NULL: text/plain and
 * anything else that is not HTML is shown verbatim. */
int   html_to_text(const char *html, size_t n, const char *base, const char *ctype, struct doc *d);
/* Plain text (a file, an error message) as a document. */
int   text_to_doc(const char *text, size_t n, struct doc *d);
void  doc_strip(const char *in, char *out, size_t cap);              /* drop the control bytes (tests, --dump) */

/* Line layout: d->text wrapped to `width` cells. Each line is [start,end)
 * in d->text with the bold/link state it starts in. */
struct line { size_t start, end; uint8_t attr; };
struct layout { struct line *lines; int n, cap; };

void  layout_free(struct layout *l);
int   layout_doc(const struct doc *d, int width, struct layout *l);
/* The line as it is shown: UTF-8 with SGR sequences when `sgr`, else plain. */
size_t layout_line(const struct doc *d, const struct layout *l, int i, int sgr, char *out, size_t cap);

#endif

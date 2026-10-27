#ifndef ALTEREGO_JSON_H
#define ALTEREGO_JSON_H

#include <stddef.h>

/* A growable string and the two halves of JSON the agent needs: escaping
 * text into a JSON string literal, and reading string values back out of the
 * API's response. Not a JSON parser; the response shape is fixed and known. */

struct sbuf {
    char *p;
    size_t len, cap;
};

void sbuf_init(struct sbuf *b);
void sbuf_free(struct sbuf *b);
int  sbuf_add(struct sbuf *b, const char *s, size_t n);   /* 0 ok, -1 ENOMEM */
int  sbuf_adds(struct sbuf *b, const char *s);
int  sbuf_addf(struct sbuf *b, const char *fmt, ...);
int  sbuf_add_json_string(struct sbuf *b, const char *s);  /* with the quotes */

/* Decode a JSON string literal whose first character (after the opening
 * quote) is at `src`. Returns a malloc'd UTF-8 string and sets *endp to the
 * closing quote, or NULL on malformed input. */
char *json_unquote(const char *src, const char **endp);

/* Find `"key":` at top level of a flat object region and return a pointer to
 * the first character of its value, or NULL. Byte search: safe because a raw
 * `"key":` sequence cannot occur inside a JSON string (quotes are escaped). */
const char *json_value(const char *json, const char *key);

#endif

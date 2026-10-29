/* Host-side checks for the browser's pieces that need no network: URL
 * resolution, HTML to text, the layout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "web.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void check_resolve(const char *base, const char *ref, const char *want)
{
    char out[4096];
    url_resolve(base, ref, out, sizeof out);
    CHECK(strcmp(out, want) == 0, "resolve(%s, %s) = '%s', want '%s'", base, ref, out, want);
}

static void test_url(void)
{
    struct url u;
    CHECK(url_parse("https://Example.com/a/b?c=1#x", &u) == 0, "parse");
    CHECK(strcmp(u.scheme, "https") == 0 && strcmp(u.host, "example.com") == 0 && u.port == 0 && strcmp(u.path, "/a/b?c=1") == 0,
          "parts %s %s %d %s", u.scheme, u.host, u.port, u.path);
    CHECK(url_parse("http://h:8080", &u) == 0 && u.port == 8080 && strcmp(u.path, "/") == 0, "port and empty path '%s'", u.path);
    CHECK(url_parse("http://[::1]:81/x", &u) == 0 && strcmp(u.host, "::1") == 0 && u.port == 81, "ipv6 host '%s' %d", u.host, u.port);
    CHECK(url_parse("example.com/x", &u) < 0, "no scheme");
    CHECK(url_parse("about:home", &u) == 0 && strcmp(u.path, "home") == 0, "about");
    CHECK(url_parse("file:///etc/hosts", &u) == 0 && strcmp(u.path, "/etc/hosts") == 0, "file '%s'", u.path);
    const char *b = "http://a/b/c/d;p?q";                       /* RFC 3986 5.4 */
    check_resolve(b, "g", "http://a/b/c/g");
    check_resolve(b, "./g", "http://a/b/c/g");
    check_resolve(b, "g/", "http://a/b/c/g/");
    check_resolve(b, "/g", "http://a/g");
    check_resolve(b, "//g", "http://g");
    check_resolve(b, "?y", "http://a/b/c/d;p?y");
    check_resolve(b, "g?y", "http://a/b/c/g?y");
    check_resolve(b, "#s", "http://a/b/c/d;p?q");
    check_resolve(b, "g#s", "http://a/b/c/g");
    check_resolve(b, "", "http://a/b/c/d;p?q");
    check_resolve(b, ".", "http://a/b/c/");
    check_resolve(b, "..", "http://a/b/");
    check_resolve(b, "../g", "http://a/b/g");
    check_resolve(b, "../..", "http://a/");
    check_resolve(b, "../../g", "http://a/g");
    check_resolve(b, "../../../g", "http://a/g");
    check_resolve(b, "/./g", "http://a/g");
    check_resolve(b, "g/../h", "http://a/b/c/h");
    check_resolve(b, "https://x.org/p#f", "https://x.org/p");
    check_resolve("https://h:8443/a/", "b", "https://h:8443/a/b");
    check_resolve("https://h/a", "  /x ", "https://h/x ");
    char enc[64];
    url_encode("日本 語&x", enc, sizeof enc);
    CHECK(strcmp(enc, "%E6%97%A5%E6%9C%AC+%E8%AA%9E%26x") == 0, "encode '%s'", enc);
}

static void render(const char *html, const char *base, char *out, size_t cap, struct doc *d)
{
    html_to_text(html, strlen(html), base, "text/html", d);
    doc_strip(d->text, out, cap);
}

static void test_html(void)
{
    struct doc d;
    char out[4096];
    render("<html><head><title> A &amp; B </title><style>p{}</style><script>x<y</script></head>"
           "<body><h1>Head</h1><p>one <b>two</b>\n three</p><p>four<br>five</p></body></html>", "http://h/", out, sizeof out, &d);
    CHECK(strcmp(d.title, "A & B") == 0, "title '%s'", d.title);
    CHECK(strcmp(out, "Head\n\none two three\n\nfour\nfive\n") == 0, "blocks:\n%s", out);
    doc_free(&d);

    render("<p>see <a href=\"/x\">here</a> and <a href='y?z=1#f'>there</a><a href=\"#top\">top</a><a href=\"javascript:void(0)\">js</a>.</p>",
           "http://h/a/b", out, sizeof out, &d);
    CHECK(strcmp(out, "see here[1] and there[2]topjs.\n") == 0, "links:\n%s", out);
    CHECK(d.nlinks == 2 && strcmp(d.links[0], "http://h/x") == 0 && strcmp(d.links[1], "http://h/a/y?z=1") == 0, "link urls %d", d.nlinks);
    CHECK(strchr(d.text, TX_LINK_ON) && strchr(d.text, TX_LINK_OFF), "link markers");
    doc_free(&d);

    render("<ul><li>a</li><li>b<ul><li>c</li></ul></li></ul><ol><li>x</li><li>y</li></ol>", "http://h/", out, sizeof out, &d);
    CHECK(strcmp(out, "・a\n・b\n  ・c\n\n1. x\n2. y\n") == 0, "lists:\n%s", out);
    doc_free(&d);

    render("<pre>a  b\n  c</pre><p>&#x3042;&#12356;&nbsp;&copy;&unknown;&lt;</p>", "http://h/", out, sizeof out, &d);
    CHECK(strcmp(out, "a  b\n  c\n\nあい ©&unknown;<\n") == 0, "pre and entities:\n%s", out);
    doc_free(&d);

    render("<img alt=\"写真\" src=x><img src=y><table><tr><th>k</th><th>v</th></tr><tr><td>1</td><td>2</td></tr></table>", "http://h/", out, sizeof out, &d);
    CHECK(strcmp(out, "[画像: 写真]\n\nk  v\n1  2\n") == 0, "img and table:\n%s", out);
    doc_free(&d);

    render("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=Shift_JIS\"><p>x</p>", "http://h/", out, sizeof out, &d);
    CHECK(strcmp(d.charset, "shift_jis") == 0, "charset '%s'", d.charset);
    doc_free(&d);

    render("<!-- <p>no</p> --><!DOCTYPE html><p>a < b</p><P CLASS=\"x\">B</P>", "http://h/", out, sizeof out, &d);
    CHECK(strcmp(out, "a < b\n\nB\n") == 0, "comments, lone <, upper case:\n%s", out);
    doc_free(&d);

    /* not HTML: shown as is */
    html_to_text("hello <b>world</b>\n", 19, "http://h/", "text/plain", &d);
    CHECK(strcmp(d.text, "hello <b>world</b>\n") == 0, "plain '%s'", d.text);
    doc_free(&d);
}

static void line(const struct doc *d, const struct layout *l, int i, char *out, size_t cap)
{
    char raw[1024];
    layout_line(d, l, i, 0, raw, sizeof raw);
    doc_strip(raw, out, cap);
}

static void test_layout(void)
{
    struct doc d;
    struct layout l;
    char s[256];
    text_to_doc("aaa bbb ccc ddd\n日本語の文章です\nsupercalifragilistic x\n", strlen("aaa bbb ccc ddd\n日本語の文章です\nsupercalifragilistic x\n"), &d);
    layout_doc(&d, 10, &l);
    CHECK(l.n == 7, "lines %d", l.n);
    line(&d, &l, 0, s, sizeof s); CHECK(strcmp(s, "aaa bbb") == 0, "l0 '%s'", s);
    line(&d, &l, 1, s, sizeof s); CHECK(strcmp(s, "ccc ddd") == 0, "l1 '%s'", s);
    line(&d, &l, 2, s, sizeof s); CHECK(strcmp(s, "日本語の文") == 0, "l2 '%s'", s);
    line(&d, &l, 3, s, sizeof s); CHECK(strcmp(s, "章です") == 0, "l3 '%s'", s);
    line(&d, &l, 4, s, sizeof s); CHECK(strcmp(s, "supercalif") == 0, "l4 '%s'", s);
    line(&d, &l, 5, s, sizeof s); CHECK(strcmp(s, "ragilistic") == 0, "l5 '%s'", s);
    line(&d, &l, 6, s, sizeof s); CHECK(strcmp(s, "x") == 0, "l6 '%s'", s);
    layout_free(&l);
    doc_free(&d);

    /* attributes survive a wrap and become SGR */
    const char *html = "<p>plain <a href=\"/u\">a link that wraps</a> end</p>";
    html_to_text(html, strlen(html), "http://h/", "text/html", &d);
    layout_doc(&d, 12, &l);
    CHECK(l.n == 3, "wrapped link lines %d", l.n);
    char sgr[256];
    layout_line(&d, &l, 0, 1, sgr, sizeof sgr);
    CHECK(strcmp(sgr, "plain \x1b[0;4;36ma link\x1b[0m") == 0, "sgr0 '%s'", sgr);
    layout_line(&d, &l, 1, 1, sgr, sizeof sgr);
    CHECK(strcmp(sgr, "\x1b[0;4;36mthat\x1b[0m") == 0, "sgr1 '%s'", sgr);
    layout_line(&d, &l, 2, 1, sgr, sizeof sgr);
    CHECK(strcmp(sgr, "\x1b[0;4;36mwraps[1]\x1b[0m end\x1b[0m") == 0, "sgr2 '%s'", sgr);
    layout_free(&l);
    doc_free(&d);
}

int main(void)
{
    test_url();
    test_html();
    test_layout();
    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("web: all checks passed");
    return 0;
}

/* Host-side checks for the screen's pieces that need no framebuffer:
 * the terminal emulator, romaji-to-kana, and the font file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "font.h"
#include "kana.h"
#include "term.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void feed_romaji(struct kana *k, const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s; s++) o += kana_feed(k, (unsigned char)*s, out + o, cap - o);
    o += kana_flush(k, out + o, cap - o);
    out[o] = '\0';
}

static void test_kana(void)
{
    struct kana k;
    char out[128];
    static const char *const cases[][2] = {
        {"konnnichiha", "こんにちは"}, {"konnichiha", "こんいちは"}, {"tsukareta", "つかれた"}, {"kyou", "きょう"}, {"gakkou", "がっこう"},
        {"shinbun", "しんぶん"}, {"kanni", "かんい"}, {"hon", "ほん"}, {"nn", "ん"}, {"n'a", "んあ"},
        {"TESUTO", "テスト"}, {"ramen-", "らめんー"}, {"ka,ta.", "か、た。"}, {"nani?", "なに？"},
        {"ja", "じゃ"}, {"xtu", "っ"}, {"fa", "ふぁ"}, {"q", "q"}, {"123", "123"}, {"[a]", "「あ」"},
        {"ko-hi-", "こーひー"}, {"matte", "まって"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        kana_reset(&k);
        feed_romaji(&k, cases[i][0], out, sizeof out);
        CHECK(strcmp(out, cases[i][1]) == 0, "kana %s -> %s (want %s)", cases[i][0], out, cases[i][1]);
    }
    kana_reset(&k);
    kana_feed(&k, 'k', out, sizeof out);
    CHECK(k.n == 1, "k pending");
    CHECK(kana_backspace(&k) == 1 && k.n == 0, "backspace clears pending");
    CHECK(kana_backspace(&k) == 0, "nothing pending");
}

static void feed(struct term *t, const char *s) { term_feed(t, s, strlen(s)); }

static void test_term(void)
{
    struct term t;
    char row[256];
    term_init(&t, 10, 3);
    feed(&t, "わたし> hi\r\n");
    term_row(&t, 0, row, sizeof row);
    CHECK(strcmp(row, "わたし> hi") == 0, "row0 '%s'", row);
    CHECK(t.cx == 0 && t.cy == 1, "cursor %d,%d", t.cx, t.cy);
    /* backspace over a wide character, as the tty echoes it: \b \b */
    feed(&t, "あい\b \b");
    term_row(&t, 1, row, sizeof row);
    CHECK(strcmp(row, "あ") == 0, "after erase '%s'", row);
    CHECK(t.cx == 2, "cursor after erase %d", t.cx);
    /* wrap and scroll */
    feed(&t, "\r\n0123456789abc");
    term_row(&t, 2, row, sizeof row);
    CHECK(strcmp(row, "abc") == 0, "scrolled row2 '%s'", row);
    term_row(&t, 1, row, sizeof row);
    CHECK(strcmp(row, "0123456789") == 0, "scrolled row1 '%s'", row);
    /* CSI: clear line, cursor moves, clear screen */
    feed(&t, "\x1b[2K\x1b[Hxy\x1b[K");
    term_row(&t, 0, row, sizeof row);
    CHECK(strcmp(row, "xy") == 0, "csi row0 '%s'", row);
    feed(&t, "\x1b[2J");
    term_row(&t, 1, row, sizeof row);
    CHECK(row[0] == '\0' && t.cx == 0 && t.cy == 0, "cleared");
    /* a UTF-8 sequence split across two feeds */
    feed(&t, "\xe3\x81");
    feed(&t, "\x82");
    term_row(&t, 0, row, sizeof row);
    CHECK(strcmp(row, "あ") == 0, "split utf8 '%s'", row);
    /* a wide character at the last column wraps as a whole */
    feed(&t, "\x1b[2J" "123456789か");
    term_row(&t, 0, row, sizeof row);
    CHECK(strcmp(row, "123456789") == 0, "row0 before wrap '%s'", row);
    term_row(&t, 1, row, sizeof row);
    CHECK(strcmp(row, "か") == 0, "wrapped wide '%s'", row);
    term_free(&t);
}

static void test_font(const char *path)
{
    struct font f;
    int n = font_load(&f, path);
    CHECK(n > 7000, "font glyphs %d", n);
    if (n < 0) return;
    CHECK(font_cells(&f, 'A') == 1, "A narrow");
    CHECK(font_cells(&f, 0x3042) == 2, "あ wide");
    CHECK(font_cells(&f, 0x4E9C) == 2, "亜 wide");
    CHECK(font_cells(&f, 0xFF71) == 1, "ｱ half-width");
    CHECK(font_cells(&f, 0x1F600) == 1, "unknown emoji guessed narrow... %d", font_cells(&f, 0x1F600));
    struct canvas c;
    canvas_init(&c, 32, 16);
    font_draw(&f, &c, 0, 0, 0x3042, 0xFFFFFF, 0);
    int lit = 0;
    for (int i = 0; i < 32 * 16; i++) if (c.px[i]) lit++;
    CHECK(lit > 20 && lit < 200, "あ draws %d pixels", lit);
    CHECK(font_text_width(&f, "あa") == 24, "text width");
    canvas_free(&c);
    font_free(&f);
}

int main(int argc, char **argv)
{
    test_kana();
    test_term();
    test_font(argc > 1 ? argv[1] : "font.hex");
    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("screen: all checks passed");
    return 0;
}

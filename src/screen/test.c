/* Host-side checks for the screen's pieces that need no framebuffer:
 * the terminal emulator, romaji-to-kana, and the font file. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canvas.h"
#include "dict.h"
#include "font.h"
#include "ime.h"
#include "kana.h"
#include "paint.h"
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
    feed(&t, "ゆうや> hi\r\n");
    term_row(&t, 0, row, sizeof row);
    CHECK(strcmp(row, "ゆうや> hi") == 0, "row0 '%s'", row);
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
    /* SGR: bold, underline + color, reverse, reset; unknown 38;5;n leaves the default color */
    feed(&t, "\x1b[2J\x1b[1mA\x1b[0;4;36mB\x1b[7mC\x1b[0mD\x1b[38;5;200;1mE");
    CHECK(t.cells[0].attr == ATTR_BOLD && t.cells[0].fg == 0, "bold %d %d", t.cells[0].attr, t.cells[0].fg);
    CHECK(t.cells[1].attr == ATTR_UNDERLINE && t.cells[1].fg == 7, "underline cyan %d %d", t.cells[1].attr, t.cells[1].fg);
    CHECK(t.cells[2].attr == (ATTR_UNDERLINE | ATTR_REVERSE) && t.cells[2].fg == 7, "reverse %d", t.cells[2].attr);
    CHECK(t.cells[3].attr == 0 && t.cells[3].fg == 0, "reset %d %d", t.cells[3].attr, t.cells[3].fg);
    CHECK(t.cells[4].attr == ATTR_BOLD && t.cells[4].fg == 0, "256-color skipped, bold kept %d %d", t.cells[4].attr, t.cells[4].fg);
    term_row(&t, 0, row, sizeof row);
    CHECK(strcmp(row, "ABCDE") == 0, "sgr text '%s'", row);
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

static void type_keys(struct ime *m, const char *keys, char *out, size_t cap)
{
    size_t o = 0;
    for (; *keys; keys++) {
        char t[2] = { *keys, 0 };
        if (*keys == '\n') t[0] = '\r';
        o += ime_key(m, t, out + o, cap - o);
    }
    out[o] = '\0';
}

static void test_dict(const char *path)
{
    struct dict d;
    CHECK(dict_load(&d, path) == 0, "dictionary %s loads", path);
    if (!d.idx) return;
    CHECK(d.n > 100000, "dictionary entries %d", d.n);
    const char *c = dict_lookup(&d, "かんじ");
    CHECK(c && strncmp(c, "漢字/", 6) == 0, "かんじ -> %s", c ? c : "(null)");
    CHECK(dict_lookup(&d, "たべr") != NULL, "okuri-ari たべr");
    CHECK(dict_lookup(&d, "ぞんざいなよみ") == NULL, "unknown reading");
    CHECK(dict_okuri_letter(0x308B) == 'r', "る -> r");      /* る */
    CHECK(dict_okuri_letter(0x3057) == 's', "し -> s");      /* し */
    CHECK(dict_okuri_letter(0x3093) == 'n', "ん -> n");      /* ん */

    char cands[IME_CANDS][DICT_CAND];
    size_t seg;
    int n = dict_convert(&d, "かんじ", cands, IME_CANDS, &seg);
    CHECK(n >= 3 && strcmp(cands[0], "漢字") == 0 && seg == 9, "かんじ converts to %s (seg %zu)", cands[0], seg);
    CHECK(strcmp(cands[n - 2], "かんじ") == 0 && strcmp(cands[n - 1], "カンジ") == 0, "hiragana and katakana last: %s %s", cands[n - 2], cands[n - 1]);
    n = dict_convert(&d, "たべる", cands, IME_CANDS, &seg);
    CHECK(n >= 1 && strcmp(cands[0], "食べる") == 0 && seg == 9, "たべる -> %s", cands[0]);
    n = dict_convert(&d, "はなした", cands, IME_CANDS, &seg);
    CHECK(n >= 1 && strcmp(cands[0], "話した") == 0, "はなした -> %s", cands[0]);
    n = dict_convert(&d, "わたしは", cands, IME_CANDS, &seg);
    CHECK(n >= 1 && strcmp(cands[0], "私は") == 0 && seg == 12, "わたしは -> %s (seg %zu)", cands[0], seg);
    n = dict_convert(&d, "かんじ、", cands, IME_CANDS, &seg);
    CHECK(n >= 1 && strcmp(cands[0], "漢字") == 0 && seg == 9, "かんじ、 -> %s + rest (seg %zu)", cands[0], seg);
    n = dict_convert(&d, "ぞんざいなよみ", cands, IME_CANDS, &seg);
    CHECK(n >= 1, "unknown still offers something: %s", cands[0]);

    /* the IME on top: type, Space, Enter */
    struct ime m;
    char out[512];
    ime_init(&m, &d);
    type_keys(&m, "kannji", out, sizeof out);
    CHECK(out[0] == '\0' && ime_active(&m), "preedit holds かんじ, nothing sent");
    char shown[256]; size_t conv;
    ime_shown(&m, shown, sizeof shown, &conv);
    CHECK(strcmp(shown, "かんじ") == 0 && conv == 0, "shown %s", shown);
    type_keys(&m, " ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &conv);
    CHECK(m.converting && strcmp(shown, "漢字") == 0 && conv == 6, "after Space: %s", shown);
    type_keys(&m, " ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &conv);
    CHECK(strcmp(shown, "幹事") == 0, "second candidate: %s", shown);
    type_keys(&m, "\n", out, sizeof out);
    CHECK(strcmp(out, "幹事") == 0 && !ime_active(&m), "Enter commits %s", out);
    type_keys(&m, "\n", out, sizeof out);
    CHECK(strcmp(out, "\r") == 0, "Enter alone sends Enter");
    /* a phrase: 漢字 + 、, converted in two steps; a digit picks; Backspace cancels */
    type_keys(&m, "kannji, ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &conv);
    CHECK(strcmp(shown, "漢字、") == 0 && conv == 6, "漢字 + 、 shown: %s", shown);
    type_keys(&m, "\n", out, sizeof out);
    CHECK(strcmp(out, "漢字") == 0 && strcmp(m.pre, "、") == 0, "commit 漢字, 、 stays (%s / %s)", out, m.pre);
    type_keys(&m, "\n", out, sizeof out);
    CHECK(strcmp(out, "、\r") == 0 && !ime_active(&m), "Enter commits the rest and sends: %s", out);
    type_keys(&m, "gakusei 2", out, sizeof out);
    CHECK(strlen(out) > 0 && !m.converting, "digit picks candidate 2: %s", out);
    type_keys(&m, "\n", out, sizeof out);
    type_keys(&m, "nani \x7f", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &conv);
    CHECK(!m.converting && strcmp(shown, "なに") == 0, "Backspace cancels conversion: %s", shown);
    type_keys(&m, "\x1b", out, sizeof out);
    CHECK(!ime_active(&m), "Escape clears");
    type_keys(&m, "desu.\n", out, sizeof out);
    CHECK(strcmp(out, "です。\r") == 0, "punctuation stays in the preedit, Enter sends: %s", out);
    /* no dictionary: Space offers hiragana / katakana */
    ime_init(&m, NULL);
    type_keys(&m, "tesuto ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &conv);
    CHECK(strcmp(shown, "てすと") == 0, "no dict, first: %s", shown);
    type_keys(&m, " \n", out, sizeof out);
    CHECK(strcmp(out, "テスト") == 0, "no dict, katakana: %s", out);
    type_keys(&m, "\n", out, sizeof out);
    CHECK(strcmp(out, "\r") == 0, "then Enter sends");
    dict_free(&d);
}

static void test_paint(const char *fontpath)
{
    struct font f;
    if (font_load(&f, fontpath) < 0) { CHECK(0, "paint: no font"); return; }
    struct paint p;
    CHECK(paint_init(&p, 600, 100) == 0, "paint init");   /* wide enough for the whole toolbar */
    CHECK(paint_pixel(&p, 50, 50) == 0xFFFFFF, "paper is white");
    /* a stroke in the default black, medium brush, below the toolbar */
    paint_press(&p, &f, 20, PAINT_TB_H + 20, NULL);
    paint_move(&p, 60, PAINT_TB_H + 20);
    paint_release(&p);
    CHECK(paint_pixel(&p, 40, 20) == 0x000000 && paint_pixel(&p, 40, 22) == 0x000000, "stroke drawn");
    CHECK(paint_pixel(&p, 40, 30) == 0xFFFFFF, "brush stays narrow");
    CHECK(p.strokes == 1 && !p.drawing, "one stroke, released");
    /* the third palette square (red), then a dot */
    paint_press(&p, &f, 6 + 2 * 20 + 5, 10, NULL);
    CHECK(p.color == paint_palette[2] && !p.drawing, "palette click picks red, draws nothing");
    paint_press(&p, &f, 100, PAINT_TB_H + 50, NULL);
    paint_release(&p);
    CHECK(paint_pixel(&p, 100, 50) == paint_palette[2], "red dot");
    /* the eraser over the first stroke */
    int ex = 0;
    { /* find the eraser button: the first toolbar hit that switches the tool */
        for (int x = 300; x < 600 && p.tool != PT_ERASER; x += 2) { paint_press(&p, &f, x, 10, NULL); ex = x; }
    }
    CHECK(p.tool == PT_ERASER, "eraser found at x=%d", ex);
    paint_press(&p, &f, 40, PAINT_TB_H + 20, NULL);
    paint_release(&p);
    CHECK(paint_pixel(&p, 40, 20) == 0xFFFFFF, "erased");
    /* save, then clear */
    char dir[] = "/tmp/alterego-paint-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    CHECK(paint_save(&p, dir) == 0 && strstr(p.msg, "保存した: ") == p.msg, "save '%s'", p.msg);
    char path[256];
    snprintf(path, sizeof path, "%s", p.msg + strlen("保存した: "));
    FILE *pf = fopen(path, "rb");
    CHECK(pf != NULL, "saved file exists");
    if (pf) { char hdr[16] = ""; if (fgets(hdr, sizeof hdr, pf)) CHECK(strcmp(hdr, "P6\n") == 0, "ppm header '%s'", hdr); fclose(pf); remove(path); }
    rmdir(dir);
    paint_clear(&p);
    CHECK(paint_pixel(&p, 100, 50) == 0xFFFFFF && p.strokes == 0, "cleared");
    /* draws without touching pixels outside its box */
    struct canvas c;
    canvas_init(&c, 300, 200);
    canvas_fill(&c, 0, 0, 300, 200, 0x123456);
    paint_draw(&p, &c, &f, 10, 10);
    CHECK(c.px[5 * 300 + 5] == 0x123456 && c.px[(10 + PAINT_TB_H + 5) * 300 + 15] == 0xFFFFFF, "drawn in place");
    canvas_free(&c);
    paint_free(&p);
    font_free(&f);
}

int main(int argc, char **argv)
{
    test_kana();
    test_term();
    test_paint(argc > 1 ? argv[1] : "font.hex");
    test_font(argc > 1 ? argv[1] : "font.hex");
    test_dict(argc > 2 ? argv[2] : "dict.txt");
    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("screen: all checks passed");
    return 0;
}

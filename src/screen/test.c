/* Host-side checks for the screen's pieces that need no framebuffer:
 * the terminal emulator, romaji-to-kana, and the font file. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canvas.h"
#include "dict.h"
#include "face.h"
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

static char oscs[4][64];
static int noscs;
static void count_osc(struct term *t, const char *osc, void *arg)
{
    (void)t; (void)arg;
    if (noscs < 4) snprintf(oscs[noscs], sizeof oscs[0], "%s", osc);
    noscs++;
}

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
    /* OSC strings are kept for the desktop (BEL or ESC \\ ends them), DCS/APC are not */
    feed(&t, "\x1b[2JX\x1b]7700;open;paint\x07Y");
    CHECK(t.osc_ready && strcmp(t.osc, "7700;open;paint") == 0, "osc bel '%s'", t.osc);
    t.osc_ready = 0;
    feed(&t, "\x1b]7700;open;web;http://a/b\x1b\\Z");
    CHECK(t.osc_ready && strcmp(t.osc, "7700;open;web;http://a/b") == 0, "osc st '%s'", t.osc);
    t.osc_ready = 0;
    feed(&t, "\x1bPsomething\x1b\\");
    CHECK(!t.osc_ready, "dcs not reported");
    term_row(&t, 0, row, sizeof row);
    CHECK(strcmp(row, "XYZ") == 0, "osc hidden '%s'", row);
    /* with a callback, two OSCs in one read both arrive (an open request and the face's state on one line) */
    t.on_osc = count_osc;
    t.osc_arg = oscs;
    noscs = 0;
    feed(&t, "a\x1b]7700;open;paint\x1b\\\x1b]7700;face;mood=1\x1b\\b");
    CHECK(noscs == 2 && strcmp(oscs[0], "7700;open;paint") == 0 && strcmp(oscs[1], "7700;face;mood=1") == 0, "two oscs: %d", noscs);
    CHECK(!t.osc_ready, "osc_ready stays clear with a callback");
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
    char shown[512]; size_t so, sl;
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(strcmp(shown, "かんじ") == 0 && sl == 0, "shown %s", shown);
    type_keys(&m, " ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(m.converting && strcmp(shown, "漢字") == 0 && so == 0 && sl == 6, "after Space: %s", shown);
    type_keys(&m, " ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(strcmp(shown, "幹事") == 0, "second candidate: %s", shown);
    type_keys(&m, "\n", out, sizeof out);
    CHECK(strcmp(out, "幹事") == 0 && !ime_active(&m), "Enter commits %s", out);
    type_keys(&m, "\n", out, sizeof out);
    CHECK(strcmp(out, "\r") == 0, "Enter alone sends Enter");
    /* a phrase is cut into 文節 and converted at once; punctuation is a segment of its own */
    ime_init(&m, &d);                                              /* (forget the 幹事 it just learned) */
    type_keys(&m, "kannji, ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(strcmp(shown, "漢字、") == 0 && so == 0 && sl == 6 && m.nsegs == 2, "漢字 + 、 shown: %s (%d segments)", shown, m.nsegs);
    type_keys(&m, "\n", out, sizeof out);
    CHECK(strcmp(out, "漢字、") == 0 && !ime_active(&m), "Enter commits both: %s", out);
    type_keys(&m, "gakusei 2", out, sizeof out);
    CHECK(strlen(out) > 0 && !m.converting, "digit picks candidate 2 and commits the last segment: %s", out);
    type_keys(&m, "\n", out, sizeof out);
    type_keys(&m, "nani \x7f", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(!m.converting && strcmp(shown, "なに") == 0, "Backspace cancels conversion: %s", shown);
    type_keys(&m, "\x1b", out, sizeof out);
    CHECK(!ime_active(&m), "Escape clears");
    type_keys(&m, "desu.\n", out, sizeof out);
    CHECK(strcmp(out, "です。\r") == 0, "punctuation stays in the preedit, Enter sends: %s", out);

    /* 文節: a sentence converts as a whole; ←/→ move, Space changes one segment */
    ime_init(&m, &d);
    char cut[512];
    ime_convert_text(&m, "きょうはいいてんきですね", cut, sizeof cut);
    CHECK(strcmp(cut, "今日は|いい|天気ですね") == 0, "きょうはいいてんきですね -> %s", cut);
    ime_convert_text(&m, "わたしはがくせいです", cut, sizeof cut);
    CHECK(strcmp(cut, "私は|学生です") == 0, "わたしはがくせいです -> %s", cut);
    ime_convert_text(&m, "ほんをよむ", cut, sizeof cut);
    CHECK(strcmp(cut, "本を|読む") == 0, "ほんをよむ -> %s", cut);
    ime_convert_text(&m, "きのうはともだちとえいがをみにいきました", cut, sizeof cut);
    CHECK(strcmp(cut, "昨日は|友達と|映画を|見に行きました") == 0, "a long sentence -> %s", cut);
    ime_convert_text(&m, "はなした", cut, sizeof cut);
    CHECK(strcmp(cut, "話した") == 0, "okurigana: はなした -> %s", cut);
    ime_convert_text(&m, "たべました", cut, sizeof cut);
    CHECK(strcmp(cut, "食べました") == 0, "たべました -> %s", cut);
    ime_convert_text(&m, "ぺいんとをひらいて", cut, sizeof cut);
    CHECK(strcmp(cut, "ペイントを|開いて") == 0, "a loanword and a verb: %s", cut);
    ime_convert_text(&m, "こーひーがすき", cut, sizeof cut);
    CHECK(strcmp(cut, "コーヒーが|好き") == 0, "ー makes katakana: %s", cut);
    ime_convert_text(&m, "ありがとう", cut, sizeof cut);
    CHECK(strcmp(cut, "ありがとう") == 0, "a hiragana word stays: %s", cut);
    type_keys(&m, "honnwoyomu ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(strcmp(shown, "本を読む") == 0 && so == 0 && sl == 6, "shown %s, first segment selected", shown);
    ime_key(&m, "\x1b[C", out, sizeof out);                        /* → : the second segment */
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(so == 6 && sl == 6 && m.seg == 1, "→ selects 読む (off %zu len %zu)", so, sl);
    type_keys(&m, " ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(strncmp(shown, "本を", 6) == 0 && strcmp(shown + 6, "読む") != 0, "Space changes only that segment: %s", shown);
    ime_key(&m, "\x1b[D", out, sizeof out);                        /* ← back */
    CHECK(m.seg == 0, "← goes back");
    type_keys(&m, "\x7f", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(!m.converting && strcmp(shown, "ほんをよむ") == 0, "Backspace: the kana again: %s", shown);
    type_keys(&m, "\x1b", out, sizeof out);
    /* Shift+←/→ change where a segment ends; the rest is cut again */
    type_keys(&m, "kyouhaii ", out, sizeof out);
    ime_cut(&m, cut, sizeof cut);
    CHECK(strcmp(cut, "今日は|いい") == 0, "cut: %s", cut);
    ime_key(&m, "\x1b[1;2D", out, sizeof out);                     /* Shift+←: 今日は -> 今日 */
    ime_cut(&m, cut, sizeof cut);
    CHECK(strncmp(cut, "今日|", 7) == 0 && m.nsegs >= 2, "Shift+← shrinks the segment: %s", cut);
    ime_key(&m, "\x1b[1;2C", out, sizeof out);                     /* Shift+→: back */
    ime_cut(&m, cut, sizeof cut);
    CHECK(strcmp(cut, "今日は|いい") == 0, "Shift+→ stretches it again: %s", cut);
    type_keys(&m, "\x1b\x1b", out, sizeof out);                     /* leave the conversion, then drop the kana */
    CHECK(!ime_active(&m), "nothing left");

    /* learning: a choice comes first next time, and survives in the file */
    char dir[] = "/tmp/alterego-ime-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    char learn[300];
    snprintf(learn, sizeof learn, "%s/ime.txt", dir);
    ime_set_learning(&m, learn);
    ime_convert_text(&m, "えをみて", cut, sizeof cut);
    CHECK(strcmp(cut, "絵を|見て") == 0, "えをみて -> %s", cut);
    type_keys(&m, "kannji \n", out, sizeof out);                   /* 漢字: the first candidate, learned anyway */
    type_keys(&m, "kannji  \n", out, sizeof out);                  /* 幹事: chosen */
    CHECK(strcmp(out, "幹事") == 0, "chose 幹事: %s", out);
    type_keys(&m, "kannji ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
    CHECK(strcmp(shown, "幹事") == 0, "the choice comes first now: %s", shown);
    type_keys(&m, "\x1b\x1b", out, sizeof out);
    type_keys(&m, "kannjiwoyomu \n", out, sizeof out);
    CHECK(strcmp(out, "幹事を読む") == 0, "the learned word with a particle: %s", out);
    struct ime m2;
    ime_init(&m2, &d);
    ime_set_learning(&m2, learn);
    CHECK(m2.nlearned >= 2, "learned entries loaded: %d", m2.nlearned);
    type_keys(&m2, "kannji ", out, sizeof out);
    ime_shown(&m2, shown, sizeof shown, &so, &sl);
    CHECK(strcmp(shown, "幹事") == 0, "and still first after a restart: %s", shown);
    ime_free(&m2);
    ime_free(&m);
    unlink(learn);
    rmdir(dir);

    /* no dictionary: Space offers hiragana / katakana */
    ime_init(&m, NULL);
    type_keys(&m, "tesuto ", out, sizeof out);
    ime_shown(&m, shown, sizeof shown, &so, &sl);
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
    const char *path = p.last;
    CHECK(strncmp(path, dir, strlen(dir)) == 0, "last path '%s'", path);
    FILE *pf = fopen(path, "rb");
    CHECK(pf != NULL, "saved file exists");
    if (pf) { char hdr[16] = ""; if (fgets(hdr, sizeof hdr, pf)) CHECK(strcmp(hdr, "P6\n") == 0, "ppm header '%s'", hdr); fclose(pf); remove(path); }
    /* 「見せる」: saves and asks the desktop to show the picture; nothing to show on an empty sheet */
    int sx = 0, want = PAINT_NONE;
    for (int x = ex + 2; x < 600 && want != PAINT_SHOW; x += 2) {       /* 全部消す lies on the way: draw again after it */
        if (p.strokes == 0) { paint_press(&p, &f, 100, PAINT_TB_H + 50, NULL); paint_release(&p); }
        want = paint_press(&p, &f, x, 10, dir); sx = x;
    }
    CHECK(want == PAINT_SHOW && strncmp(p.last, dir, strlen(dir)) == 0, "show found at x=%d, saved '%s'", sx, p.last);
    CHECK(strcmp(p.msg, "AlterEgo に見せた") == 0, "show msg '%s'", p.msg);
    remove(p.last);
    rmdir(dir);
    paint_clear(&p);
    CHECK(paint_press(&p, &f, sx, 10, dir) == PAINT_NONE && strstr(p.msg, "まだ何も") == p.msg, "nothing to show '%s'", p.msg);
    CHECK(paint_pixel(&p, 100, 50) == 0xFFFFFF && p.strokes == 0, "cleared");
    /* paint_run: AlterEgo's drawing commands, in 1/1000 of the height (100 px here) */
    uint32_t pen_color = p.color; int pen_size = p.size;
    CHECK(paint_run(&p, "k E53935 C 300 300 100 k 1E88E5 s 20 l 4000 500 5000 500 p 3 1000 100 1200 100 1200 300") == 6, "run commands");
    CHECK(paint_pixel(&p, 30, 30) == 0xE53935 && paint_pixel(&p, 30, 36) == 0xE53935 && paint_pixel(&p, 30, 42) == 0xFFFFFF, "filled circle r=10");
    CHECK(paint_pixel(&p, 450, 50) == 0x1E88E5 && paint_pixel(&p, 450, 56) == 0xFFFFFF, "thick line");
    CHECK(paint_pixel(&p, 110, 10) == 0x1E88E5 && paint_pixel(&p, 120, 20) == 0x1E88E5, "polyline");
    CHECK(p.strokes == 1 && p.color == pen_color && p.size == pen_size, "run keeps the user's pen");
    CHECK(paint_run(&p, "e") == 1 && p.strokes == 0 && paint_pixel(&p, 30, 30) == 0xFFFFFF, "e empties the sheet");
    CHECK(paint_run(&p, "x 1 2") == 0, "unknown command draws nothing");
    /* i: a saved picture's ink pasted into a box (the earlier red dot, saved again) */
    paint_press(&p, &f, 6 + 2 * 20 + 5, 10, NULL);
    paint_press(&p, &f, 100, PAINT_TB_H + 50, NULL);
    paint_release(&p);
    char dir2[] = "/tmp/alterego-paste-XXXXXX";
    CHECK(mkdtemp(dir2) != NULL && paint_save(&p, dir2) == 0, "save for paste");
    paint_clear(&p);
    char cmd[640];
    snprintf(cmd, sizeof cmd, "i %s 3000 100 3500 600", p.last);        /* units of h/1000 = 0.1 px: the box 300..350 x 10..60 */
    CHECK(paint_run(&p, cmd) == 1 && p.strokes == 1, "paste runs");
    CHECK(paint_pixel(&p, 325, 35) == paint_palette[2] && paint_pixel(&p, 100, 50) == 0xFFFFFF, "pasted ink scaled into the box");
    remove(p.last);
    rmdir(dir2);
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

/* The face: the agent's state is parsed, the expression follows it, the
 * mood, the body and the silence, and it draws. */
static void test_face(const char *fontpath)
{
    struct font f;
    if (font_load(&f, fontpath) < 0) { CHECK(0, "font %s", fontpath); return; }
    struct face fc;
    long now = 500000;
    face_init(&fc, now);
    CHECK(strcmp(face_expr(&fc, now), "calm") == 0, "newborn calm: %s", face_expr(&fc, now));
    face_set(&fc, "mood=45;bond=70;stage=2;body=0;expr=;say=0", now);
    CHECK(fc.mood == 45 && fc.bond == 70 && fc.stage == 2 && fc.body == 0, "parsed %d %d %d %d", fc.mood, fc.bond, fc.stage, fc.body);
    CHECK(strcmp(face_expr(&fc, now), "happy") == 0, "good mood: %s", face_expr(&fc, now));
    face_set(&fc, "mood=-50;bond=70;stage=2;body=0;expr=;say=0", now);
    CHECK(strcmp(face_expr(&fc, now), "sad") == 0, "bad mood: %s", face_expr(&fc, now));
    /* a told expression holds a few seconds, then the mood shows again; the mouth moves while it speaks */
    face_set(&fc, "mood=-50;bond=70;stage=2;body=0;expr=puzzled;say=10", now);
    CHECK(strcmp(face_expr(&fc, now + 1000), "puzzled") == 0, "told: %s", face_expr(&fc, now + 1000));
    CHECK(face_talking(&fc, now + 300) && !face_talking(&fc, now + 2000), "talking for 10 characters");
    CHECK(strcmp(face_expr(&fc, now + 5000), "sad") == 0, "after the told one: %s", face_expr(&fc, now + 5000));
    /* a key: listening; the body: tired, dizzy; silence: drowsy, asleep */
    face_typing(&fc, now + 5000);
    CHECK(strcmp(face_expr(&fc, now + 5500), "listen") == 0, "typing: %s", face_expr(&fc, now + 5500));
    face_set(&fc, "mood=0;bond=0;stage=1;body=2;expr=;say=0", now + 9000);
    CHECK(strcmp(face_expr(&fc, now + 9000), "tired") == 0, "tired: %s", face_expr(&fc, now + 9000));
    face_set(&fc, "mood=0;bond=0;stage=1;body=3;expr=;say=0", now + 9000);
    CHECK(strcmp(face_expr(&fc, now + 9000), "dizzy") == 0, "dizzy: %s", face_expr(&fc, now + 9000));
    face_set(&fc, "mood=0;bond=0;stage=1;body=0;expr=;say=0", now + 9000);
    CHECK(strcmp(face_expr(&fc, now + 9000 + 100000), "drowsy") == 0, "100 s: %s", face_expr(&fc, now + 109000));
    CHECK(strcmp(face_expr(&fc, now + 9000 + 200000), "sleep") == 0, "200 s: %s", face_expr(&fc, now + 209000));
    /* out-of-range values are clamped, junk is ignored */
    face_set(&fc, "mood=999;bond=-5;stage=9;body=7;expr=averyveryverylongname;nonsense;=;say=-3", now);
    CHECK(fc.mood == 100 && fc.bond == 0 && fc.stage == 3 && fc.body == 3, "clamped %d %d %d %d", fc.mood, fc.bond, fc.stage, fc.body);
    /* it draws: skin in the middle of the head, the label under it, nothing outside its box */
    struct canvas cv;
    canvas_init(&cv, FACE_W + 20, FACE_H + 20);
    canvas_fill(&cv, 0, 0, cv.w, cv.h, 0x123456);
    face_set(&fc, "mood=50;bond=40;stage=3;body=0;expr=happy;say=0", now);
    face_look(&fc, 100, 0);
    for (int i = 0; i < 10; i++) face_tick(&fc, now + i * 80);
    face_draw(&fc, &cv, &f, 10, 10, now + 800);
    CHECK(cv.px[(size_t)(10 + 100) * cv.w + 10 + FACE_W / 2] == 0xFFE9DA, "skin at the centre: %06x", cv.px[(size_t)110 * cv.w + 10 + FACE_W / 2]);
    CHECK(strcmp(fc.label, "うれしい") == 0, "label %s", fc.label);
    CHECK(fc.look_x == 4, "eyes toward the pointer: %d", fc.look_x);
    int outside = 0;
    for (int y = 0; y < cv.h; y++) for (int x = 0; x < cv.w; x++)
        if ((x < 10 || y < 10 || x >= 10 + FACE_W || y >= 10 + FACE_H) && cv.px[(size_t)y * cv.w + x] != 0x123456) outside++;
    CHECK(outside == 0, "%d pixels outside the box", outside);
    /* the stage changes the face: a newborn has no hair over the forehead */
    face_set(&fc, "mood=0;bond=0;stage=0;body=0;expr=;say=0", now);
    face_draw(&fc, &cv, &f, 10, 10, now);
    CHECK(cv.px[(size_t)(10 + 104 - 40) * cv.w + 10 + FACE_W / 2 - 20] == 0xFFE9DA, "newborn forehead is skin");
    canvas_free(&cv);
    font_free(&f);
}

int main(int argc, char **argv)
{
    test_kana();
    test_term();
    test_paint(argc > 1 ? argv[1] : "font.hex");
    test_face(argc > 1 ? argv[1] : "font.hex");
    test_font(argc > 1 ? argv[1] : "font.hex");
    test_dict(argc > 2 ? argv[2] : "dict.txt");
    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("screen: all checks passed");
    return 0;
}

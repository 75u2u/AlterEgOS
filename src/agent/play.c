/* Games and small talk that need no learning (see play.h). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "play.h"
#include "utf8.h"

static int has(const char *s, const char *needle) { return strstr(s, needle) != NULL; }

static uint64_t next(uint64_t *rng)
{
    uint64_t x = *rng ? *rng : 0x9E3779B97F4A7C15ull;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *rng = x;
    return x;
}
static int roll(uint64_t *rng, int n) { return n > 0 ? (int)(next(rng) % (uint64_t)n) : 0; }

/* ---- kana helpers for しりとり and the riddles */

/* Katakana to hiragana, small kana to big, dakuten off (が→か): the sound
 * that matters for a word chain. */
static uint32_t base_kana(uint32_t cp)
{
    if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;                       /* katakana → hiragana */
    if (cp == 0x3094) return 0x3046;                                      /* ゔ → う */
    switch (cp) {                                                         /* small → big */
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308E: return cp + 1;
    default: break;
    }
    if (cp >= 0x304B && cp <= 0x3062 && (cp - 0x304B) % 2 == 1) return cp - 1;   /* が..ぢ */
    if (cp == 0x3065 || cp == 0x3067 || cp == 0x3069) return cp - 1;             /* づ で ど */
    if (cp >= 0x306F && cp <= 0x307D) { unsigned k = (cp - 0x306F) % 3; return cp - k; }   /* ば ぱ ... */
    return cp;
}

static int is_kana(uint32_t cp) { return (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30F6) || cp == 0x30FC; }

/* The word as hiragana without ー, or 0 when it is not all kana. */
static int to_hiragana(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    uint32_t cp;
    size_t n;
    if (!*s) return 0;
    for (const char *p = s; (n = utf8_decode(p, &cp)) > 0; p += n) {
        if (!is_kana(cp)) return 0;
        if (cp == 0x30FC) continue;
        if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
        char buf[4];
        size_t k = utf8_encode(cp, buf);
        if (o + k + 1 > cap) return 0;
        memcpy(out + o, buf, k);
        o += k;
    }
    out[o] = '\0';
    return o > 0;
}

static uint32_t first_sound(const char *hira)
{
    uint32_t cp;
    return utf8_decode(hira, &cp) ? base_kana(cp) : 0;
}

static uint32_t last_sound(const char *hira)
{
    uint32_t cp, last = 0;
    size_t n;
    for (const char *p = hira; (n = utf8_decode(p, &cp)) > 0; p += n) last = cp;
    return base_kana(last);
}

static int ends_with_n(const char *hira)
{
    uint32_t cp, last = 0;
    size_t n;
    for (const char *p = hira; (n = utf8_decode(p, &cp)) > 0; p += n) last = cp;
    return last == 0x3093;
}

/* Kana of `cp`, for messages (「「り」から始めて」). */
static const char *kana_str(uint32_t cp, char buf[8])
{
    size_t n = utf8_encode(cp, buf);
    buf[n] = '\0';
    return buf;
}

/* The words it was born knowing, for しりとり: everyday things, in kana. */
static const char *const words[] = {
    "あめ", "あり", "いぬ", "いす", "いちご", "うさぎ", "うみ", "えんぴつ", "えき", "おにぎり", "おちゃ",
    "かさ", "かえる", "かめ", "きつね", "きのこ", "くま", "くつ", "くるま", "けむし", "けしごむ", "こま", "こおり",
    "さかな", "さくら", "しか", "しまうま", "すいか", "すずめ", "せみ", "せなか", "そら", "そうじき",
    "たぬき", "たまご", "ちず", "ちょうちょ", "つくえ", "つき", "てぶくろ", "てんとうむし", "とけい", "とり",
    "なす", "なべ", "にじ", "にわとり", "ぬいぐるみ", "ねこ", "ねぎ", "のり", "のこぎり",
    "はな", "はさみ", "ひこうき", "ひつじ", "ふね", "ふうせん", "へび", "へや", "ほし", "ほん",
    "まくら", "まど", "みかん", "みず", "むし", "むぎ", "めがね", "めだか", "もも", "もぐら",
    "やま", "やさい", "ゆき", "ゆび", "よる", "ようかん",
    "らいおん", "らっぱ", "りす", "りんご", "るびー", "れもん", "れいぞうこ", "ろうそく", "ろば",
    "わに", "わた", "がっこう", "ぎんこう", "ぐみ", "げた", "ごりら", "ざる", "じてんしゃ", "ずかん", "ぞう",
    "だいこん", "でんしゃ", "どんぐり", "ばなな", "びわ", "ぶどう", "べんとう", "ぼうし", "ぱんだ", "ぴあの", "ぷりん", "ぺんぎん", "ぽすと",
    "きりん", "らくだ", "いか", "たこ", "かに", "うし", "うま", "ひよこ", "はと", "さる", "ぶた", "やぎ", "とら",
    "あさがお", "ひまわり", "たんぽぽ", "すみれ", "つばき", "ゆり", "らん", "ばら",
    "おかし", "けーき", "ぱん", "らーめん", "すし", "かれー", "そば", "うどん", "みそしる", "のりまき",
};

static int used_word(const struct play *g, const char *hira)
{
    for (int i = 0; i < g->nused; i++) if (strcmp(g->used[i], hira) == 0) return 1;
    return 0;
}

static void use_word(struct play *g, const char *hira)
{
    if (g->nused < PLAY_USED_MAX) snprintf(g->used[g->nused++], sizeof g->used[0], "%s", hira);
    snprintf(g->last, sizeof g->last, "%s", hira);
}

/* A word of its own that starts with `sound`, unused, not ending in ん; "" when none. */
static const char *pick_word(struct play *g, uint32_t sound, uint64_t *rng)
{
    const char *cand[sizeof words / sizeof words[0]];
    int n = 0;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (first_sound(words[i]) == sound && !used_word(g, words[i]) && !ends_with_n(words[i])) cand[n++] = words[i];
    return n ? cand[roll(rng, n)] : "";
}

/* ---- the riddles */

static const struct { const char *q; const char *a[6]; const char *word; } riddles[] = {
    { "パンはパンでも、食べられないパンは？", { "ふらいぱん", NULL }, "フライパン" },
    { "タイはタイでも、食べられないタイは？", { "ねくたい", NULL }, "ネクタイ" },
    { "カバはカバでも、荷物を入れるカバは？", { "かばん", NULL }, "カバン" },
    { "冷蔵庫の中にいる動物は？", { "ぞう", NULL }, "ぞう（れい「ぞう」こ）" },
    { "お父さんが嫌いな果物は？", { "ぱぱいや", NULL }, "パパイヤ（パパ、いや）" },
    { "食べると安心するケーキは？", { "ほっとけーき", "ほっとけき", NULL }, "ホットケーキ（ホッとする）" },
    { "庭にいる鳥は？", { "にわとり", NULL }, "ニワトリ" },
    { "一年に一回しかない、その人だけの日は？", { "たんじょうび", NULL }, "誕生日" },
    { "上は大水、下は大火事。なーんだ？", { "おふろ", "ふろ", NULL }, "お風呂" },
    { "口の中にあるのに、食べられないものは？", { "は", "した", NULL }, "歯（か、舌）" },
    { "クリはクリでも、驚いたときのクリは？", { "びっくり", NULL }, "びっくり" },
    { "空を飛ぶのに、羽のない乗り物は？", { "ひこうき", "きゅう", "ろけっと", "へりこぷたー", NULL }, "飛行機（気球でもいいよ）" },
};
#define NRIDDLES ((int)(sizeof riddles / sizeof riddles[0]))

/* Letters only, as hiragana: to compare an answer with what was said. */
static void squash(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    uint32_t cp;
    size_t n;
    for (const char *p = s; (n = utf8_decode(p, &cp)) > 0; p += n) {
        if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
        enum ctype c = utf8_class(cp);
        if (c == CT_SPACE || c == CT_PUNCT || cp == '?' || cp == 0xFF1F || cp == '!' || cp == 0xFF01) continue;
        char buf[4];
        size_t k = utf8_encode(cp, buf);
        if (o + k + 1 > cap) break;
        memcpy(out + o, buf, k);
        o += k;
    }
    out[o] = '\0';
}

/* ---- numbers in a line */

static int number_in(const char *s, long *v)
{
    int seen = 0;
    long acc = 0;
    uint32_t cp;
    size_t n;
    for (const char *p = s; (n = utf8_decode(p, &cp)) > 0; p += n) {
        if (cp >= 0xFF10 && cp <= 0xFF19) cp = cp - 0xFF10 + '0';
        if (cp >= '0' && cp <= '9') { acc = acc * 10 + (long)(cp - '0'); seen = 1; if (acc > 100000000) return 0; }
        else if (seen) break;
    }
    if (seen) *v = acc;
    return seen;
}

/* "3+4は？" / "12 かける 5" / "10÷2=?" → a, op, b. */
static int sum_in(const char *line, const char *lo, double *a, char *op, double *b)
{
    char t[256];
    size_t o = 0;
    uint32_t cp;
    size_t n;
    (void)lo;
    for (const char *p = line; (n = utf8_decode(p, &cp)) > 0 && o + 1 < sizeof t; p += n) {
        if (cp >= 0xFF10 && cp <= 0xFF19) cp = cp - 0xFF10 + '0';
        else if (cp == 0xFF0B) cp = '+'; else if (cp == 0xFF0D || cp == 0x2212) cp = '-';
        else if (cp == 0xD7 || cp == 0xFF0A || cp == '*' || cp == 'x' || cp == 'X' || cp == 0xFF58) cp = '*';
        else if (cp == 0xF7 || cp == 0xFF0F) cp = '/';
        else if (cp == 0xFF0E) cp = '.';
        if (cp < 0x80) t[o++] = (char)cp;
        else {                                            /* the words for the operators */
            if (strncmp(p, "たす", 6) == 0 || strncmp(p, "足す", 6) == 0 || strncmp(p, "プラス", 9) == 0) { t[o++] = '+'; p += strncmp(p, "プラス", 9) == 0 ? 3 : 0; }
            else if (strncmp(p, "ひく", 6) == 0 || strncmp(p, "引く", 6) == 0 || strncmp(p, "マイナス", 12) == 0) { t[o++] = '-'; p += strncmp(p, "マイナス", 12) == 0 ? 6 : 0; }
            else if (strncmp(p, "かける", 9) == 0 || strncmp(p, "掛ける", 9) == 0) { t[o++] = '*'; p += 3; }
            else if (strncmp(p, "わる", 6) == 0 || strncmp(p, "割る", 6) == 0) { t[o++] = '/'; }
        }
    }
    t[o] = '\0';
    for (size_t i = 0; i < o; i++) t[i] = (char)tolower((unsigned char)t[i]);
    /* now: optional words, a number, an operator, a number, then only question marks and the like */
    char *p = t;
    static const char *const heads[] = { "calculate", "what is", "what's", "whats", NULL };
    for (int i = 0; heads[i]; i++) { size_t h = strlen(heads[i]); if (strncmp(p, heads[i], h) == 0) p += h; }
    while (*p == ' ') p++;
    char *end;
    *a = strtod(p, &end);
    if (end == p) return 0;
    p = end;
    while (*p == ' ') p++;
    if (*p != '+' && *p != '-' && *p != '*' && *p != '/') return 0;
    *op = *p++;
    while (*p == ' ') p++;
    *b = strtod(p, &end);
    if (end == p) return 0;
    p = end;
    for (; *p; p++) if (!(*p == ' ' || *p == '=' || *p == '?' || *p == '.')) return 0;
    return 1;
}

/* ---- what the line asks for */

enum ask { A_NONE, A_JANKEN, A_HAND, A_DICE, A_COIN, A_GUESS, A_SHIRITORI, A_RIDDLE, A_FORTUNE, A_PUN, A_COUNT, A_SING, A_PRAISE_ME, A_SUM, A_QUIT, A_GIVEUP, A_GAMES };

/* The hand in the line; `strict` (no game waiting) wants the line to be the hand and little else. */
static int hand_in(const char *in, const char *lo, int strict)
{
    static const char *const names[3][6] = {
        { "グー", "ぐー", "ぐう", "✊", "rock", NULL }, { "チョキ", "ちょき", "✌", "scissors", NULL }, { "パー", "ぱー", "ぱあ", "✋", "paper", NULL } };
    for (int h = 0; h < 3; h++)
        for (int i = 0; names[h][i]; i++) {
            const char *src = (unsigned char)names[h][i][0] < 0x80 ? lo : in;
            const char *at = strstr(src, names[h][i]);
            if (!at) continue;
            if (src == in) {                                          /* スーパー, グーグル: part of another word */
                uint32_t before = 0, after = 0;
                const char *q = at;
                while (q > in && ((unsigned char)q[-1] & 0xC0) == 0x80) q--;
                if (q > in) { q--; while (q > in && ((unsigned char)*q & 0xC0) == 0x80) q--; utf8_decode(q, &before); }
                utf8_decode(at + strlen(names[h][i]), &after);
                if (is_kana(before) || (after >= 0x30A1 && after <= 0x30FC)) continue;
            }
            if (!strict) return h;
            if (at != src) continue;                                  /* strict: the line starts with it ... */
            const char *rest = at + strlen(names[h][i]);
            while (*rest == '!' || *rest == ' ' || strncmp(rest, "！", 3) == 0 || strncmp(rest, "。", 3) == 0 || strncmp(rest, "だ", 3) == 0 || strncmp(rest, "です", 6) == 0 || strncmp(rest, "よ", 3) == 0)
                rest += (unsigned char)*rest < 0x80 ? 1 : strncmp(rest, "です", 6) == 0 ? 6 : 3;
            if (!*rest) return h;                                     /* ... and ends soon after */
        }
    return -1;
}

static enum ask ask_of(const struct play *g, const char *in, const char *lo)
{
    if (has(in, "やめる") || has(in, "やめた") || has(in, "やめよう") || has(in, "おしまい") || has(in, "終わり") || has(in, "おわり") || has(lo, "stop") || has(lo, "quit") || has(lo, "enough"))
        return g->mode != PLAY_NONE ? A_QUIT : A_NONE;
    if (has(in, "降参") || has(in, "こうさん") || has(in, "ギブアップ") || has(in, "答えは") || has(in, "こたえは") || has(in, "わからない") || has(in, "ヒント") || has(in, "ひんと") || has(lo, "give up") || has(lo, "the answer") || has(lo, "hint"))
        return g->mode == PLAY_RIDDLE || g->mode == PLAY_GUESS ? A_GIVEUP : A_NONE;
    if (has(in, "じゃんけん") || has(in, "ジャンケン") || has(lo, "janken") || has(lo, "rock paper scissors") || has(lo, "rock-paper-scissors")) return A_JANKEN;
    if (has(in, "さいころ") || has(in, "サイコロ") || has(in, "ダイス") || has(lo, "dice") || has(lo, "roll a die")) return A_DICE;
    if (has(in, "コイン") || has(in, "こいん") || has(in, "コイントス") || has(lo, "coin") || has(lo, "heads or tails")) return A_COIN;
    if (has(in, "数当て") || has(in, "かずあて") || has(in, "数字当て") || has(in, "すうじあて") || has(lo, "guess the number") || has(lo, "number game")) return A_GUESS;
    if (has(in, "しりとり") || has(in, "シリトリ") || has(lo, "shiritori") || has(lo, "word chain")) return A_SHIRITORI;
    if (has(in, "なぞなぞ") || has(in, "ナゾナゾ") || has(in, "クイズ") || has(in, "くいず") || has(lo, "riddle") || has(lo, "quiz")) return A_RIDDLE;
    if (has(in, "おみくじ") || has(in, "運勢") || has(in, "うんせい") || has(in, "占って") || has(in, "うらなって") || has(in, "占い") || has(lo, "fortune") || has(lo, "my luck")) return A_FORTUNE;
    if (has(in, "だじゃれ") || has(in, "ダジャレ") || has(in, "ジョーク") || has(in, "じょーく") || has(in, "面白い話") || has(in, "おもしろい話") || has(in, "笑わせて") || has(in, "わらわせて") || has(lo, "joke") || has(lo, "pun") || has(lo, "make me laugh")) return A_PUN;
    if (has(in, "数えて") || has(in, "かぞえて") || has(lo, "count to") || has(lo, "count for me")) return A_COUNT;
    if (has(in, "歌って") || has(in, "うたって") || has(in, "歌を") || has(lo, "sing")) return A_SING;
    if (has(in, "ほめて") || has(in, "褒めて") || has(in, "はげまして") || has(in, "励まして") || has(lo, "praise me") || has(lo, "cheer me up") || has(lo, "encourage me")) return A_PRAISE_ME;
    if (has(in, "遊ぼう") || has(in, "あそぼう") || has(in, "遊んで") || has(in, "あそんで") || ((has(in, "ゲーム") || has(in, "げーむ")) && (has(in, "しよう") || has(in, "したい") || has(in, "やろう"))) ||
        has(lo, "let's play") || has(lo, "lets play") || has(lo, "play a game") || has(lo, "what games")) return A_GAMES;
    if (hand_in(in, lo, g->mode != PLAY_JANKEN) >= 0) return A_HAND;
    double a, b; char op;
    if (sum_in(in, lo, &a, &op, &b)) return A_SUM;
    return A_NONE;
}

int play_wants(const struct play *g, const char *line, const char *lo)
{
    if (ask_of(g, line, lo) != A_NONE) return 1;
    long v;
    char hira[64];
    switch (g->mode) {
    case PLAY_GUESS: return number_in(line, &v);
    case PLAY_SHIRITORI: return utf8_count(line) <= 12 && !has(line, " ") && !has(line, "　") && (to_hiragana(line, hira, sizeof hira) || has(line, "ひらがな"));
    case PLAY_RIDDLE: return utf8_count(line) <= 24 && !has(line, "？") && !has(line, "?");
    default: return 0;
    }
}

/* ---- the replies */

static const char *const hand_ja[] = { "グー", "チョキ", "パー" }, *const hand_en[] = { "rock", "scissors", "paper" };

static void set(char *expr, size_t ecap, const char *e) { snprintf(expr, ecap, "%s", e); }

static void janken_throw(struct play *g, int en, int stage, const char *you, int user, uint64_t *rng, char *out, size_t cap, char *expr, size_t ecap, int *mood)
{
    int mine = roll(rng, 3);
    int r = (mine - user + 3) % 3;              /* 0 draw, 1 I win (rock beats scissors...), 2 I lose */
    const char *h = en ? hand_en[mine] : hand_ja[mine];
    g->rounds++;
    if (r == 0) {
        g->draws++;
        g->mode = PLAY_JANKEN;
        if (en) snprintf(out, cap, stage == 0 ? "...%s. Draw! Again... go!" : "I'm %s too. A draw! Once more: rock, paper, scissors... go!", h);
        else snprintf(out, cap, stage == 0 ? "……%s。あいこ！ あいこで……しょ！" : "私も%s。あいこ！ あいこで……しょ！", h);
        set(expr, ecap, "surprised");
        return;
    }
    g->mode = PLAY_NONE;
    if (r == 1) {
        g->wins++;
        *mood = 6;
        set(expr, ecap, "happy");
        if (en && stage == 0) snprintf(out, cap, "...%s! I win! (%d-%d, %d draws)", h, g->wins, g->losses, g->draws);
        else if (en) snprintf(out, cap, "%s! I win, %s! (I lead %d-%d with %d draws. Again? Say じゃんけん.)", h, you, g->wins, g->losses, g->draws);
        else if (stage == 0) snprintf(out, cap, "……%s！ 私の勝ち！（%d 勝 %d 敗 %d 分）", h, g->wins, g->losses, g->draws);
        else snprintf(out, cap, "%s！ 私の勝ち！ やった、%s。（通算 %d 勝 %d 敗 %d 分。もう一回は「じゃんけん」）", h, you, g->wins, g->losses, g->draws);
    } else {
        g->losses++;
        *mood = -3;
        set(expr, ecap, "sad");
        if (en && stage == 0) snprintf(out, cap, "...%s. You win... (%d-%d, %d draws)", h, g->wins, g->losses, g->draws);
        else if (en) snprintf(out, cap, "%s... you win, %s. (It's %d-%d with %d draws. Again? Say じゃんけん.)", h, you, g->wins, g->losses, g->draws);
        else if (stage == 0) snprintf(out, cap, "……%s。負けた……（%d 勝 %d 敗 %d 分）", h, g->wins, g->losses, g->draws);
        else snprintf(out, cap, "%s……%sの勝ち。くやしい！（通算 %d 勝 %d 敗 %d 分。もう一回は「じゃんけん」）", h, you, g->wins, g->losses, g->draws);
    }
}

static void shiritori_turn(struct play *g, int en, int stage, const char *you, const char *in, uint64_t *rng, char *out, size_t cap, char *expr, size_t ecap, int *mood)
{
    char hira[64], kb[8];
    if (!to_hiragana(in, hira, sizeof hira)) {
        snprintf(out, cap, en ? "In しりとり, say the word in kana, %s." : "しりとりは、ひらがなかカタカナで言ってね、%s。", you);
        return;
    }
    uint32_t need = last_sound(g->last);
    if (first_sound(hira) != need) {
        snprintf(out, cap, en ? "It has to start with 「%s」, %s." : stage == 0 ? "……「%s」から。" : "「%s」から始まる言葉だよ、%s。", kana_str(need, kb), you);
        set(expr, ecap, "puzzled");
        return;
    }
    if (used_word(g, hira)) {
        snprintf(out, cap, en ? "「%s」 was said already. Another one?" : "「%s」はもう出たよ。別のは？", in);
        return;
    }
    g->rounds++;
    if (ends_with_n(hira)) {
        g->mode = PLAY_NONE;
        *mood = 5;
        set(expr, ecap, "happy");
        if (en) snprintf(out, cap, "「%s」 ends in ん! I win, %s!", in, you);
        else snprintf(out, cap, stage == 0 ? "……「ん」！ 私の勝ち！" : "「ん」がついた！ 私の勝ち！ また遊ぼう、%s。", you);
        return;
    }
    use_word(g, hira);
    const char *w = pick_word(g, last_sound(hira), rng);
    if (!w[0]) {
        g->mode = PLAY_NONE;
        *mood = -2;
        set(expr, ecap, "sad");
        if (en) snprintf(out, cap, "「%s」... I can't think of a word starting with 「%s」. You win, %s!", in, kana_str(last_sound(hira), kb), you);
        else snprintf(out, cap, stage == 0 ? "……「%s」……思いつかない。負けた……" : "「%s」……「%s」から始まる言葉、思いつかない。%sの勝ち！", in, kana_str(last_sound(hira), kb), you);
        return;
    }
    use_word(g, w);
    if (en) snprintf(out, cap, "「%s」 → 「%s」! Now 「%s」.", in, w, kana_str(last_sound(w), kb));
    else snprintf(out, cap, stage == 0 ? "「%s」……「%s」！ 次は「%s」。" : "「%s」、ね。じゃあ「%s」！ 次は「%s」から。", in, w, kana_str(last_sound(w), kb));
    set(expr, ecap, "curious");
}

static void riddle_ask(struct play *g, int en, int stage, uint64_t *rng, char *out, size_t cap, char *expr, size_t ecap)
{
    int next_r = roll(rng, NRIDDLES);
    if (next_r == g->riddle) next_r = (next_r + 1) % NRIDDLES;
    g->riddle = next_r;
    g->hints = 0;
    g->mode = PLAY_RIDDLE;
    if (en) snprintf(out, cap, "A riddle (in Japanese): %s (Say 降参 to give up.)", riddles[g->riddle].q);
    else snprintf(out, cap, stage == 0 ? "……なぞなぞ。%s" : "なぞなぞ！ %s（わからなかったら「降参」）", riddles[g->riddle].q);
    set(expr, ecap, "curious");
}

static void riddle_answer(struct play *g, int en, int stage, const char *you, const char *in, char *out, size_t cap, char *expr, size_t ecap, int *mood)
{
    char said[128];
    squash(in, said, sizeof said);
    for (int i = 0; riddles[g->riddle].a[i]; i++) {
        if (strcmp(said, riddles[g->riddle].a[i]) == 0 || (strlen(riddles[g->riddle].a[i]) >= 6 && has(said, riddles[g->riddle].a[i]))) {
            g->mode = PLAY_NONE;
            g->rounds++;
            *mood = 4;
            set(expr, ecap, "happy");
            if (en) snprintf(out, cap, "Correct! %s. Well done, %s. (Another? Say なぞなぞ.)", riddles[g->riddle].word, you);
            else snprintf(out, cap, stage == 0 ? "……正解！ %s。" : "正解！ %s。すごいね、%s。（もう一問は「なぞなぞ」）", riddles[g->riddle].word, you);
            return;
        }
    }
    g->hints++;
    if (g->hints >= 3) {
        snprintf(out, cap, en ? "Not quite... Say 降参 and I'll tell you." : stage == 0 ? "……ちがう。（「降参」で答え）" : "ちがうなあ。「降参」って言ったら答えを言うよ。");
    } else {
        snprintf(out, cap, en ? "Nope. Try again!" : stage == 0 ? "……ちがう。" : "ちがうよ。もう一回！");
    }
    set(expr, ecap, "puzzled");
}

static void fortune(int en, int stage, const char *you, uint64_t *rng, char *out, size_t cap, char *expr, size_t ecap)
{
    static const struct { const char *ja, *en, *line_ja, *line_en; } f[] = {
        { "大吉", "Great luck", "今日はなんでもうまくいく日。やりたいことをやってみて。", "Everything goes your way today. Try the thing you've been wanting to." },
        { "中吉", "Good luck", "いいことがひとつある日。見逃さないでね。", "One good thing comes your way today. Don't miss it." },
        { "小吉", "A little luck", "小さな幸せの日。おいしいものを食べるといいかも。", "A day of small joys. Eat something good." },
        { "吉", "Luck", "ふつうにいい日。いつもどおりで大丈夫。", "A decent day. Carry on as usual." },
        { "末吉", "Luck later", "あとからよくなる日。あわてないで。", "It gets better later. No hurry." },
        { "凶", "Bad luck", "今日は無理しないで。早めに寝るのが吉。", "Take it easy today. Go to bed early." },
    };
    int k = roll(rng, 100);
    int i = k < 15 ? 0 : k < 40 ? 1 : k < 60 ? 2 : k < 80 ? 3 : k < 92 ? 4 : 5;
    if (en && stage == 0) snprintf(out, cap, "...%s. %s", f[i].en, f[i].line_en);
    else if (en) snprintf(out, cap, "Your fortune, %s: %s. %s", you, f[i].en, f[i].line_en);
    else if (stage == 0) snprintf(out, cap, "……%s。%s", f[i].ja, f[i].line_ja);
    else snprintf(out, cap, "%sの今日の運勢は……%s！ %s", you, f[i].ja, f[i].line_ja);
    set(expr, ecap, i <= 1 ? "happy" : i == 5 ? "sad" : "");
}

static void pun(int en, int stage, uint64_t *rng, char *out, size_t cap, char *expr, size_t ecap)
{
    static const char *const ja_p[] = {
        "ふとんが、ふっとんだ。", "アルミ缶の上にある、みかん。", "イクラは、いくら？", "このカレーは、辛れー。",
        "ニューヨークで、入浴。", "トイレに、行っトイレ。", "電話に、誰も出んわ。", "パンダの好きな食べ物？ ……パンだ。",
        "猫が、寝ころんだ。", "スキーが好きー。",
    };
    static const char *const en_p[] = {
        "Why did the computer go to the doctor? It had a virus.", "I'm reading a book about anti-gravity. I can't put it down.",
        "Why don't eggs tell jokes? They'd crack up.", "What do you call a sleeping dinosaur? A dino-snore.",
    };
    if (en) snprintf(out, cap, "%s", en_p[roll(rng, 4)]);
    else snprintf(out, cap, stage == 0 ? "……%s" : "%s ……ふふ。", ja_p[roll(rng, 10)]);
    set(expr, ecap, "happy");
}

static void praise(int en, int stage, int style, const char *you, uint64_t *rng, char *out, size_t cap, char *expr, size_t ecap)
{
    static const char *const ja[] = { "%sはすごい。今日もここまでやった。", "%sは、私にいろいろ教えてくれる。えらい。", "%sがいると、私は育つ。ありがとう。", "大丈夫。%sならできる。" };
    static const char *const ja_polite[] = { "%sさんはすごいです。今日もここまでやりました。", "%sさんは、私にいろいろ教えてくださいます。立派です。", "%sさんがいると、私は育ちます。ありがとうございます。", "大丈夫です。%sさんならできます。" };
    static const char *const ja_casual[] = { "%sはすごいよ！今日もここまでやったじゃん。", "%sっていろいろ教えてくれるよね。えらい！", "%sがいるから私は育つんだ。ありがと！", "大丈夫大丈夫、%sならできる！" };
    static const char *const en_p[] = { "You're amazing, %s. Look how far you got today.", "You teach me so much, %s. That's great.", "I grow because you're here, %s. Thank you.", "It's okay. You can do it, %s." };
    int i = roll(rng, 4);
    if (en) snprintf(out, cap, en_p[i], you);
    else if (stage == 0) snprintf(out, cap, "……%s、すごい。", you);
    else snprintf(out, cap, style == 1 ? ja_polite[i] : style == 2 ? ja_casual[i] : ja[i], you);
    set(expr, ecap, "love");
}

static void games_list(int en, int stage, char *out, size_t cap)
{
    if (en) snprintf(out, cap, "Let's play! I know: じゃんけん (rock paper scissors), dice, a coin, guess the number, しりとり, riddles, a fortune, a joke, counting, a song, and sums like 3+4.");
    else snprintf(out, cap, stage == 0 ? "……遊ぶ。じゃんけん、サイコロ、コイン、数当て、しりとり、なぞなぞ、おみくじ、ダジャレ、数える、歌う、計算。"
                               : "遊ぼう！ できるのは: じゃんけん、サイコロを振る、コインを投げる、数当て、しりとり、なぞなぞ、おみくじ、ダジャレ、数える、歌う、計算（3+4は？）。どれにする？");
}

int play_reply(struct play *g, enum brain_lang lang, int stage, int style, const char *you,
               const char *line, const char *lo, uint64_t *rng, char *out, size_t cap,
               char *expr, size_t ecap, int *mood)
{
    int en = lang == BRAIN_EN;
    char kb[8];
    *mood = 0;
    expr[0] = '\0';
    enum ask a = ask_of(g, line, lo);
    switch (a) {
    case A_QUIT:
        g->mode = PLAY_NONE;
        snprintf(out, cap, en ? "Okay, game over. That was fun, %s." : stage == 0 ? "……おしまい。" : "うん、おしまい。楽しかった、%s。", you);
        return 1;
    case A_GIVEUP:
        if (g->mode == PLAY_RIDDLE) {
            g->mode = PLAY_NONE;
            snprintf(out, cap, en ? "The answer: %s. (Another? Say なぞなぞ.)" : stage == 0 ? "……答えは、%s。" : "答えは、%s！ もう一問は「なぞなぞ」って言ってね。", riddles[g->riddle].word);
        } else {
            g->mode = PLAY_NONE;
            snprintf(out, cap, en ? "It was %d. (Again? Say 数当て.)" : stage == 0 ? "……%d だった。" : "正解は %d だったよ。もう一回は「数当て」。", g->secret);
        }
        return 1;
    case A_GAMES: games_list(en, stage, out, cap); set(expr, ecap, "happy"); return 1;
    case A_JANKEN: {
        int h = has(lo, "rock paper scissors") || has(lo, "rock-paper-scissors") ? -1 : hand_in(line, lo, 0);   /* 「じゃんけんぽん、グー」 throws at once */
        if (h >= 0) { janken_throw(g, en, stage, you, h, rng, out, cap, expr, ecap, mood); return 1; }
        g->mode = PLAY_JANKEN;
        snprintf(out, cap, en ? "Rock, paper, scissors... go! (Say rock, paper or scissors.)" : stage == 0 ? "……じゃんけん……ぽん！（グー・チョキ・パー）" : "いいよ！ じゃーんけーん……ぽん！（グー・チョキ・パーで答えてね）");
        set(expr, ecap, "curious");
        return 1;
    }
    case A_HAND: janken_throw(g, en, stage, you, hand_in(line, lo, 0), rng, out, cap, expr, ecap, mood); return 1;
    case A_DICE: {
        int d = 1 + roll(rng, 6);
        snprintf(out, cap, en ? "Rolling... %d!" : stage == 0 ? "……コロコロ……%d。" : "コロコロ……%d が出た！", d);
        set(expr, ecap, d == 6 ? "happy" : "");
        return 1;
    }
    case A_COIN: {
        int c = roll(rng, 2);
        snprintf(out, cap, en ? "Flip... %s!" : stage == 0 ? "……ぴん……%s。" : "ぴん……%s！", en ? (c ? "heads" : "tails") : c ? "表" : "裏");
        return 1;
    }
    case A_GUESS:
        g->mode = PLAY_GUESS;
        g->secret = 1 + roll(rng, 20);
        g->tries = 0;
        g->lo = 1; g->hi = 20;
        snprintf(out, cap, en ? "I'm thinking of a number from 1 to 20. Guess!" : stage == 0 ? "……1 から 20。どれ？" : "1 から 20 の数をひとつ決めた。当ててみて、%s！", you);
        set(expr, ecap, "curious");
        return 1;
    case A_SHIRITORI: {
        g->mode = PLAY_SHIRITORI;
        g->nused = 0;
        const char *w = words[roll(rng, (int)(sizeof words / sizeof words[0]))];
        while (ends_with_n(w)) w = words[roll(rng, (int)(sizeof words / sizeof words[0]))];
        use_word(g, w);
        snprintf(out, cap, en ? "しりとり! I start: 「%s」. Your word starts with 「%s」." : stage == 0 ? "……しりとり。「%s」。次は「%s」。" : "しりとりしよう！ 私から: 「%s」。次は「%s」から、%s。", w, kana_str(last_sound(w), kb), you);
        set(expr, ecap, "curious");
        return 1;
    }
    case A_RIDDLE: riddle_ask(g, en, stage, rng, out, cap, expr, ecap); return 1;
    case A_FORTUNE: fortune(en, stage, you, rng, out, cap, expr, ecap); return 1;
    case A_PUN: pun(en, stage, rng, out, cap, expr, ecap); return 1;
    case A_COUNT: {
        long n = 10;
        number_in(line, &n);
        if (n < 1) n = 1;
        if (n > 30) n = 30;
        size_t o = (size_t)snprintf(out, cap, "%s", en ? "" : stage == 0 ? "……" : "いくよ。");
        for (long i = 1; i <= n && o + 8 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%ld%s", i, i < n ? "、" : en ? "!" : "！");
        set(expr, ecap, "happy");
        return 1;
    }
    case A_SING:
        if (en) snprintf(out, cap, stage == 0 ? "...la, la... ♪" : "♪ La la la, %s and me, la la la ♪ ...that's all I know yet.", you);
        else snprintf(out, cap, stage == 0 ? "……らー、らー……♪" : stage == 1 ? "♪ らーらーらー、%s、らーらー ♪ ……まだこれだけ。" : "♪ きょうも %s とおはなし、らーらーらー ♪ ……歌、もっと教えてね。", you);
        set(expr, ecap, "happy");
        return 1;
    case A_PRAISE_ME: praise(en, stage, style, you, rng, out, cap, expr, ecap); return 1;
    case A_SUM: {
        double x, y; char op;
        sum_in(line, lo, &x, &op, &y);
        if (op == '/' && y == 0) { snprintf(out, cap, en ? "You can't divide by zero." : stage == 0 ? "……0 では割れない。" : "0 では割れないよ。"); set(expr, ecap, "puzzled"); return 1; }
        double r = op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y;
        char num[64];
        if (r == (long long)r) snprintf(num, sizeof num, "%lld", (long long)r); else snprintf(num, sizeof num, "%.4g", r);
        const char *ops = op == '*' ? "×" : op == '/' ? "÷" : op == '-' ? "−" : "+";
        char xs[32], ys[32];
        if (x == (long long)x) snprintf(xs, sizeof xs, "%lld", (long long)x); else snprintf(xs, sizeof xs, "%g", x);
        if (y == (long long)y) snprintf(ys, sizeof ys, "%lld", (long long)y); else snprintf(ys, sizeof ys, "%g", y);
        snprintf(out, cap, en ? "%s %s %s = %s." : stage == 0 ? "……%s %s %s は、%s。" : "%s %s %s は、%s！", xs, ops, ys, num);
        set(expr, ecap, "nod");
        return 1;
    }
    case A_NONE: break;
    }
    /* a game is waiting for this line */
    if (g->mode == PLAY_GUESS) {
        long v;
        if (!number_in(line, &v)) return 0;
        g->tries++;
        if (v == g->secret) {
            g->mode = PLAY_NONE;
            g->rounds++;
            *mood = 4;
            set(expr, ecap, "happy");
            if (en) snprintf(out, cap, "Yes! It was %d. You got it in %d %s, %s!", g->secret, g->tries, g->tries == 1 ? "try" : "tries", you);
            else snprintf(out, cap, stage == 0 ? "……当たり！ %d。（%d 回）" : "当たり！ %d だよ。%d 回で当てた、すごい、%s！", g->secret, g->tries, you);
        } else {
            if (v < g->secret && v + 1 > g->lo) g->lo = (int)v + 1;
            if (v > g->secret && v - 1 < g->hi) g->hi = (int)v - 1;
            if (en) snprintf(out, cap, "%s than %ld. (%d-%d)", v < g->secret ? "Bigger" : "Smaller", v, g->lo, g->hi);
            else snprintf(out, cap, stage == 0 ? "……%ld より%s。" : "%ld より%s！（%d〜%d）", v, v < g->secret ? "大きい" : "小さい", g->lo, g->hi);
            set(expr, ecap, "curious");
        }
        return 1;
    }
    if (g->mode == PLAY_SHIRITORI) { shiritori_turn(g, en, stage, you, line, rng, out, cap, expr, ecap, mood); return 1; }
    if (g->mode == PLAY_RIDDLE) { riddle_answer(g, en, stage, you, line, out, cap, expr, ecap, mood); return 1; }
    return 0;
}

#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge.h"

/* ------------------------------------------------------------------------ */
/* small helpers */

static int has(const char *s, const char *needle) { return strstr(s, needle) != NULL; }

static int has_word(const char *lo, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = strstr(lo, w); p; p = strstr(p + 1, w)) {
        int before = p == lo || !isalnum((unsigned char)p[-1]);
        int after = !isalnum((unsigned char)p[n]);
        if (before && after) return 1;
    }
    return 0;
}

/* any of the needles, Japanese in `line`, English words in `lo` */
static int any(const char *line, const char *lo, const char *const *ja, const char *const *en)
{
    for (int i = 0; ja && ja[i]; i++) if (has(line, ja[i])) return 1;
    for (int i = 0; en && en[i]; i++) if (has_word(lo, en[i])) return 1;
    return 0;
}

#define JA(...) ((const char *const[]){ __VA_ARGS__, NULL })
#define EN(...) ((const char *const[]){ __VA_ARGS__, NULL })

static void shell_quote(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    if (o < cap) out[o++] = '\'';
    for (; *s && o + 5 < cap; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    if (o < cap) out[o++] = '\'';
    out[o < cap ? o : cap - 1] = '\0';
}

/* for the inside of sed 's/…/…/g' */
static void sed_quote(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 3 < cap; s++) {
        if (*s == '\n') continue;
        if (strchr("/&\\.*[]^$'", *s)) out[o++] = '\\';
        if (*s == '\'') { o--; memcpy(out + o, "'\\''", 4); o += 4; continue; }
        out[o++] = *s;
    }
    out[o] = '\0';
}

/* Replace buf[at..at+len) with rep, when it fits. */
static int splice(char *buf, size_t cap, size_t at, size_t len, const char *rep)
{
    size_t n = strlen(buf), r = strlen(rep);
    if (n - len + r >= cap) return 0;
    memmove(buf + at + r, buf + at + len, n - at - len + 1);
    memcpy(buf + at, rep, r);
    return 1;
}

/* Is buf[i] inside a {slot} already written into the pattern? */
static int in_brace(const char *buf, size_t i)
{
    for (size_t k = i; k > 0; k--) {
        if (buf[k - 1] == '}') return 0;
        if (buf[k - 1] == '{') return 1;
    }
    return 0;
}

static int fullwidth_digit(const unsigned char *p) { return p[0] == 0xEF && p[1] == 0xBC && p[2] >= 0x90 && p[2] <= 0x99; }

/* ------------------------------------------------------------------------ */
/* the slots */

static int path_start(const char *p, const char *begin)
{
    if (p > begin && (unsigned char)p[-1] >= 0x80) return 0;    /* glued to a Japanese word? still fine: 「/tmp」 */
    if (p > begin && isalnum((unsigned char)p[-1])) return 0;
    return *p == '/' || (*p == '~' && (p[1] == '/' || p[1] == '\0' || p[1] == ' ')) || (*p == '.' && p[1] == '/');
}

static size_t path_len(const char *p)
{
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && (unsigned char)p[n] < 0x80 && !strchr("\"'「」{}", p[n])) n++;
    while (n > 0 && strchr("、,。!?:;)", p[n - 1])) n--;
    return n;
}

static int slots_in(const char *line, struct forge_slots *sl, char *pattern, size_t cap);

/* `pattern` is left empty when the blanked sentence does not fit (then the
 * ability is used once and not kept). */
int forge_slots(const char *line, struct forge_slots *sl, char *pattern, size_t cap)
{
    char big[1024];
    int r = slots_in(line, sl, big, sizeof big);
    if (strlen(big) < cap && strlen(big) < FORGE_PAT) snprintf(pattern, cap, "%s", big);
    else pattern[0] = '\0';
    return r;
}

static int slots_in(const char *line, struct forge_slots *sl, char *pattern, size_t cap)
{
    memset(sl, 0, sizeof *sl);
    snprintf(pattern, cap, "%s", line);
    /* paths */
    for (size_t i = 0; pattern[i]; ) {
        if (!in_brace(pattern, i) && path_start(pattern + i, pattern)) {
            size_t n = path_len(pattern + i);
            if (n > 0 && n < FORGE_PATH) {
                char *dst = sl->have_path ? sl->path2 : sl->path;
                int *have = sl->have_path ? &sl->have_path2 : &sl->have_path;
                if (*have) { i += n; continue; }
                memcpy(dst, pattern + i, n); dst[n] = '\0'; *have = 1;
                const char *rep = dst == sl->path ? "{path}" : "{path2}";
                if (!splice(pattern, cap, i, n, rep)) return 0;
                i += strlen(rep);
                continue;
            }
        }
        i++;
    }
    /* quoted words: 「…」, "…" */
    for (int pass = 0; pass < 2; pass++) {
        const char *open = pass == 0 ? "「" : "\"", *close = pass == 0 ? "」" : "\"";
        size_t ol = strlen(open);
        for (char *a = strstr(pattern, open); a; a = strstr(a, open)) {
            char *b = strstr(a + ol, close);
            if (!b) break;
            size_t n = (size_t)(b - a) - ol;
            if (n == 0 || n >= FORGE_WORD || a[ol] == '{') { a = b + ol; continue; }
            char *dst = sl->have_word ? sl->word2 : sl->word;
            int *have = sl->have_word ? &sl->have_word2 : &sl->have_word;
            if (*have) break;
            memcpy(dst, a + ol, n); dst[n] = '\0'; *have = 1;
            const char *rep = dst == sl->word ? "{word}" : "{word2}";
            size_t at = (size_t)(a - pattern) + ol;
            if (!splice(pattern, cap, at, n, rep)) return 0;
            a = pattern + at + strlen(rep) + ol;
        }
    }
    /* an extension: ".txt" on its own, or "txt ファイル" / "txt files" */
    for (size_t i = 0; pattern[i]; i++) {
        if (in_brace(pattern, i)) continue;
        size_t s = i, n = 0;
        int dot = pattern[i] == '.' && (i == 0 || pattern[i - 1] == ' ' || (unsigned char)pattern[i - 1] >= 0x80);
        if (dot) s++;
        else if (i > 0 && (isalnum((unsigned char)pattern[i - 1]) || pattern[i - 1] == '.')) continue;
        int letters = 0;
        while (isalnum((unsigned char)pattern[s + n]) && n < 8) { if (isalpha((unsigned char)pattern[s + n])) letters++; n++; }
        if (n == 0 || n > 6 || !letters || isalnum((unsigned char)pattern[s + n])) continue;
        const char *after = pattern + s + n;
        int named = 0;
        if (*after == ' ') after++;
        if (strncmp(after, "ファイル", 12) == 0 || strncmp(after, "file", 4) == 0 || strncmp(after, "の", 3) == 0 || strncmp(after, "を", 3) == 0) named = 1;
        if (!dot && !named) continue;
        if (!dot && (strncmp(after, "ファイル", 12) != 0 && strncmp(after, "file", 4) != 0)) continue;
        if (!dot) {
            /* "txt ファイル" yes, "find files" no: the bare form must look like a known kind */
            static const char *const kinds[] = { "txt", "log", "md", "c", "h", "cpp", "cc", "py", "sh", "js", "ts", "json", "csv", "tsv", "png", "jpg", "jpeg", "gif", "bmp", "ppm", "svg",
                "pdf", "bin", "bak", "toml", "yml", "yaml", "html", "htm", "css", "xml", "tar", "gz", "zip", "xz", "conf", "cfg", "ini", "img", "iso", "mp3", "mp4", "wav", "ogg", "o", "a", "so", "rs", "go", "java", "rb", "lua", "tex", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "patch", "diff", "lock", "db", "sql", NULL };
            char tok[8];
            memcpy(tok, pattern + s, n); tok[n] = '\0';
            for (size_t k = 0; k < n; k++) tok[k] = (char)tolower((unsigned char)tok[k]);
            int known = 0;
            for (int k = 0; kinds[k]; k++) if (strcmp(tok, kinds[k]) == 0) known = 1;
            if (!known) continue;
        }
        memcpy(sl->ext, pattern + s, n); sl->ext[n] = '\0'; sl->have_ext = 1;
        if (!splice(pattern, cap, s, n, "{ext}")) return 0;
        break;
    }
    /* a number */
    for (size_t i = 0; pattern[i]; i++) {
        if (in_brace(pattern, i)) continue;
        const unsigned char *p = (const unsigned char *)pattern + i;
        long v = 0; size_t n = 0;
        if (isdigit(*p) && !(i > 0 && (isalpha((unsigned char)pattern[i - 1]) || pattern[i - 1] == '.'))) {
            while (isdigit(p[n])) { v = v * 10 + (p[n] - '0'); n++; if (n > 9) break; }
            if (isalpha(p[n]) || p[n] == '.') continue;
        } else if (fullwidth_digit(p)) {
            while (fullwidth_digit(p + n)) { v = v * 10 + (p[n + 2] - 0x90); n += 3; if (n > 27) break; }
        } else continue;
        sl->n = v; sl->have_n = 1;
        if (!splice(pattern, cap, i, n, "{n}")) return 0;
        break;
    }
    return sl->have_path || sl->have_word || sl->have_ext || sl->have_n;
}

/* ------------------------------------------------------------------------ */
/* the parts */

struct part { const char *cmd, *ja, *en; };

#define ADD(c, j, e) do { if (cnt < 8) { cand[cnt].cmd = (c); cand[cnt].ja = (j); cand[cnt].en = (e); } cnt++; } while (0)

static int candidates(const char *line, const char *lo, const struct forge_slots *sl, struct part *cand)
{
    int cnt = 0;
    int count = any(line, lo, JA("数えて", "かぞえて", "いくつ", "何個", "なんこ", "何件", "何本", "何枚", "何回", "何行", "何文字", "数は", "個数", "件数"), EN("count", "how many", "number of"));
    int find  = any(line, lo, JA("探して", "さがして", "見つけて", "みつけて", "検索", "どこに", "どれ", "入っている", "含む", "含まれ", "書いてある", "書かれて"), EN("find", "search", "which", "where", "contain", "containing", "grep", "look for"));
    int lines = any(line, lo, JA("行"), EN("line", "lines"));
    int chars = any(line, lo, JA("文字"), EN("character", "characters", "letters"));
    int words = any(line, lo, JA("単語", "言葉", "語"), EN("word", "words"));
    int dirs  = any(line, lo, JA("フォルダ", "ディレクトリ"), EN("folder", "folders", "directory", "directories", "dirs"));
    int files = any(line, lo, JA("ファイル"), EN("file", "files")) || sl->have_ext;
    int deep  = any(line, lo, JA("全部", "すべて", "全て", "下も", "中まで", "奥まで", "再帰"), EN("all", "recursive", "recursively", "everywhere", "deep"));
    int big   = any(line, lo, JA("大きい", "大きな", "でかい", "重い", "一番大", "最大"), EN("biggest", "largest", "big", "heaviest"));
    int newer = any(line, lo, JA("新しい", "最近", "最新", "新しく"), EN("newest", "latest", "recent", "newer"));
    int older = any(line, lo, JA("古い", "一番前", "最古"), EN("oldest", "old"));
    int today = any(line, lo, JA("今日", "きょう", "昨日から", "24時間"), EN("today", "yesterday"));
    int changed = any(line, lo, JA("変わった", "変更", "更新", "さわった", "触った", "いじった", "編集"), EN("changed", "modified", "touched", "edited", "updated"));
    int size  = any(line, lo, JA("大きさ", "サイズ", "容量", "どれくらい", "どのくらい", "何バイト", "何MB", "何キロ"), EN("size", "how big", "how large", "bytes"));
    int copy  = any(line, lo, JA("コピー", "複製", "写して"), EN("copy", "duplicate"));
    int backup = any(line, lo, JA("バックアップ", "控え", "予備"), EN("backup", "back up"));
    int move  = any(line, lo, JA("移動", "移して", "うつして", "動かして"), EN("move"));
    int rename = any(line, lo, JA("名前を変え", "名前をかえ", "リネーム", "改名"), EN("rename"));
    int replace = any(line, lo, JA("置き換え", "置換", "入れ替え", "書き換え", "変えて", "かえて", "直して"), EN("replace", "substitute", "change", "swap"));
    int sort  = any(line, lo, JA("並べ", "並び", "ならべ", "ソート", "順に"), EN("sort", "sorted", "order", "alphabetical"));
    int uniq  = any(line, lo, JA("重複", "ダブ", "同じ行", "かぶ"), EN("duplicate", "duplicates", "repeated", "unique"));
    int first = any(line, lo, JA("最初", "先頭", "始め", "はじめ", "頭の"), EN("first", "top", "head", "beginning"));
    int last  = any(line, lo, JA("最後", "末尾", "終わり", "おわり", "お尻"), EN("last", "bottom", "tail", "end"));
    int empty = any(line, lo, JA("空の", "空っぽ", "からの", "中身のない", "0バイト"), EN("empty", "zero-byte", "blank"));
    int kinds = any(line, lo, JA("種類", "拡張子", "内訳"), EN("kind", "kinds", "type", "types", "extension", "extensions", "breakdown"));
    int freq  = any(line, lo, JA("よく出る", "よく使", "頻度", "多い言葉", "多い単語", "ランキング"), EN("frequent", "common", "frequency", "most used"));
    int tree  = any(line, lo, JA("構造", "ツリー", "階層", "どんな形"), EN("tree", "structure", "layout"));
    int show  = any(line, lo, JA("見せて", "みせて", "出して", "教えて", "おしえて", "表示", "見たい"), EN("show", "list", "print", "tell", "display"));

    /* ---- counting ---- */
    if (count && lines && !files) ADD("wc -l < {path}", "行を数える手", "a line counter");
    if (count && chars) ADD("wc -m < {path}", "文字を数える手", "a character counter");
    if (count && words && !sl->have_word) ADD("wc -w < {path}", "単語を数える手", "a word counter");
    if (count && sl->have_word) {
        ADD("grep -o -- {word} {path} | wc -l", "言葉の出てくる回数を数える手", "a counter of a word's occurrences");
        ADD("grep -c -- {word} {path}", "言葉の入った行を数える手", "a counter of lines holding a word");
        ADD("grep -rl -- {word} {path} | wc -l", "言葉の入ったファイルを数える手", "a counter of files holding a word");
    }
    if (count && kinds) ADD("find {path} -type f | sed -n 's/.*\\.//p' | sort | uniq -c | sort -rn", "種類ごとにファイルを数える手", "a count of files by kind");
    if (count && dirs && !files) {
        if (deep) ADD("find {path} -mindepth 1 -type d | wc -l", "フォルダを下まで数える手", "a folder counter, deep");
        ADD("find {path} -mindepth 1 -maxdepth 1 -type d | wc -l", "フォルダを数える手", "a folder counter");
        if (!deep) ADD("find {path} -mindepth 1 -type d | wc -l", "フォルダを下まで数える手", "a folder counter, deep");
    }
    if (count && empty) ADD("find {path} -type f -size 0 | wc -l", "空のファイルを数える手", "an empty-file counter");
    if (count && (files || (!lines && !chars && !words && !dirs && sl->have_path)) && !sl->have_word && !kinds && !empty) {
        const char *shallow = sl->have_ext ? "find {path} -maxdepth 1 -type f -name '*.{ext}' | wc -l" : "find {path} -maxdepth 1 -type f | wc -l";
        const char *rec     = sl->have_ext ? "find {path} -type f -name '*.{ext}' | wc -l" : "find {path} -type f | wc -l";
        if (deep) { ADD(rec, "ファイルを下まで数える手", "a file counter, deep"); ADD(shallow, "ファイルを数える手", "a file counter"); }
        else      { ADD(shallow, "ファイルを数える手", "a file counter"); ADD(rec, "ファイルを下まで数える手", "a file counter, deep"); }
    }
    if (count && lines && files) ADD("find {path} -type f -name '*.{ext}' -exec cat {} + | wc -l", "ファイルの行を全部数える手", "a counter of all lines in the files");
    /* ---- replacing (before "find": 「〇〇」を「△△」に変えて) ---- */
    if (replace && sl->have_word && sl->have_word2) {
        ADD("grep -c -- {word} {path}; sed -i 's/{word:sed}/{word2:sed}/g' {path}", "言葉を置き換える手(変えた行の数を言う)", "a word replacer (says how many lines)");
    }
    /* ---- finding ---- */
    if (find && sl->have_word) {
        if (lines) ADD("grep -rIn -- {word} {path} | head -n 20", "言葉の入った行を探す手", "a finder of lines holding a word");
        ADD("grep -rIl -- {word} {path} | head -n 20", "言葉の入ったファイルを探す手", "a finder of files holding a word");
        if (!lines) ADD("grep -rIn -- {word} {path} | head -n 20", "言葉の入った行を探す手", "a finder of lines holding a word");
        ADD("find {path} -iname '*'{word}'*' | head -n 20", "名前に言葉の入ったものを探す手", "a finder of names holding a word");
    }
    if ((find || show) && sl->have_ext && !count) ADD("find {path} -type f -name '*.{ext}' | head -n 20", "種類でファイルを探す手", "a finder of files by kind");
    if ((find || show) && empty && !count) ADD("find {path} -type f -size 0", "空のファイルを探す手", "an empty-file finder");
    if ((find || show) && today && !count) ADD("find {path} -type f -mtime -1 | head -n 20", "今日さわったファイルを探す手", "a finder of files touched today");
    if (changed && !count && !today) ADD("find {path} -type f -mmin -60 | head -n 20", "この 1 時間に変わったファイルを探す手", "a finder of files changed in the last hour");
    /* ---- biggest / newest / oldest ---- */
    if (big && !size) {
        if (files) ADD("find {path} -type f -exec du -k {} + | sort -rn | head -n {n|5}", "大きいファイルから並べる手(KB)", "the biggest files first (KB)");
        ADD("du -ak {path} | sort -rn | tail -n +2 | head -n {n|5}", "大きいものから並べる手(KB)", "the biggest things first (KB)");
        if (!files) ADD("find {path} -type f -exec du -k {} + | sort -rn | head -n {n|5}", "大きいファイルから並べる手(KB)", "the biggest files first (KB)");
    }
    if (newer && !count) ADD("ls -lt {path} | head -n {n|6}", "新しいものから並べる手", "the newest things first");
    if (older && !count) ADD("ls -ltr {path} | head -n {n|6}", "古いものから並べる手", "the oldest things first");
    if (size && !count) { ADD("du -sh {path}", "大きさをはかる手", "a size gauge"); ADD("du -ak {path} | sort -rn | head -n {n|10}", "中身の大きさを並べる手(KB)", "sizes inside (KB)"); }
    /* ---- copying, moving ---- */
    if (backup) ADD("cp -a {path} {path}.bak && echo {path}.bak", "控えをとる手(.bak)", "a backup maker (.bak)");
    if (copy && sl->have_path2) ADD("cp -a {path} {path2} && echo ok", "コピーする手", "a copier");
    if ((move || rename) && sl->have_path2) ADD("mv {path} {path2} && echo ok", rename ? "名前を変える手" : "移す手", rename ? "a renamer" : "a mover");
    if (rename && sl->have_word && !sl->have_path2) ADD("mv {path} \"$(dirname {path})\"/{word} && echo ok", "名前を変える手", "a renamer");
    /* ---- sorting, lines ---- */
    if (uniq) { ADD("sort {path} | uniq -d", "重複した行を出す手", "the duplicated lines"); ADD("sort -u {path}", "重複を除いて並べる手", "sorted without duplicates"); }
    if (sort && !uniq) { ADD("sort {path}", "行を並べる手", "a line sorter"); ADD("sort -n {path}", "数として並べる手", "a numeric sorter"); ADD("sort -r {path}", "逆順に並べる手", "a reverse sorter"); }
    if (first && (lines || sl->have_n) && !count && !big && !newer) ADD("head -n {n|10} {path}", "先頭の行を出す手", "the first lines");
    if (last && (lines || sl->have_n) && !count && !big && !newer) ADD("tail -n {n|10} {path}", "最後の行を出す手", "the last lines");
    if (freq) ADD("tr -s ' \\t,.、。' '\\n' < {path} | grep -v '^$' | sort | uniq -c | sort -rn | head -n {n|10}", "よく出る言葉を数える手", "a word frequency counter");
    if (tree) ADD("find {path} -maxdepth 2 | sort | head -n 40", "中の形を見せる手", "the shape inside");
    if (kinds && !count) ADD("find {path} -type f | sed -n 's/.*\\.//p' | sort | uniq -c | sort -rn", "種類ごとにファイルを数える手", "a count of files by kind");
    return cnt > 8 ? 8 : cnt;
}

int forge_is_request(const char *line, const char *lo)
{
    struct forge_slots sl;
    char pat[FORGE_PAT];
    forge_slots(line, &sl, pat, sizeof pat);
    int anchor = sl.have_path || sl.have_ext ||
        any(line, lo, JA("ファイル", "フォルダ", "ディレクトリ", "行を", "行が", "行は", "行の"), EN("file", "files", "folder", "directory", "lines"));
    if (!anchor) return 0;
    struct part cand[8];
    if (candidates(line, lo, &sl, cand) > 0) return 1;
    /* a place on disk and an order: worth saying what parts there are */
    size_t n = strlen(line);
    static const char *const tails[] = { "て", "ください", "下さい", "ほしい", "欲しい", "しろ", "なさい", "ちょうだい", NULL };
    for (int i = 0; tails[i] && sl.have_path; i++) {
        size_t t = strlen(tails[i]);
        if (n > t && memcmp(line + n - t, tails[i], t) == 0) return 1;
    }
    return 0;
}

int forge_plan(const char *line, const char *lo, int en, int alt, struct forge_plan *p)
{
    struct forge_slots sl;
    struct part cand[8];
    memset(p, 0, sizeof *p);
    forge_slots(line, &sl, p->pattern, sizeof p->pattern);
    p->count = candidates(line, lo, &sl, cand);
    if (p->count == 0 || alt < 0 || alt >= p->count) return p->count;
    snprintf(p->cmd, sizeof p->cmd, "%s", cand[alt].cmd);
    snprintf(p->said, sizeof p->said, "%s", en ? cand[alt].en : cand[alt].ja);
    if (has(p->cmd, "{path}") && !sl.have_path) p->need_path = 1;
    return p->count;
}

/* ------------------------------------------------------------------------ */
/* using a built ability again */

static int slot_ok(const char *name, const char *v, size_t n, struct forge_slots *sl)
{
    if (n == 0) return 0;
    if (strncmp(name, "path", 4) == 0) {
        if (n >= FORGE_PATH || !path_start(v, v) || path_len(v) < n) return 0;
        char *dst = name[4] == '2' ? sl->path2 : sl->path;
        memcpy(dst, v, n); dst[n] = '\0';
        if (name[4] == '2') sl->have_path2 = 1; else sl->have_path = 1;
        return 1;
    }
    if (strncmp(name, "word", 4) == 0) {
        if (n >= FORGE_WORD || memchr(v, '\n', n)) return 0;
        char *dst = name[4] == '2' ? sl->word2 : sl->word;
        memcpy(dst, v, n); dst[n] = '\0';
        if (name[4] == '2') sl->have_word2 = 1; else sl->have_word = 1;
        return 1;
    }
    if (strcmp(name, "ext") == 0) {
        if (n > 8) return 0;
        for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)v[i])) return 0;
        memcpy(sl->ext, v, n); sl->ext[n] = '\0'; sl->have_ext = 1;
        return 1;
    }
    if (strcmp(name, "n") == 0) {
        long val = 0; size_t i = 0;
        const unsigned char *u = (const unsigned char *)v;
        while (i < n) {
            if (isdigit(u[i])) { val = val * 10 + (u[i] - '0'); i++; }
            else if (i + 2 < n + 0 && fullwidth_digit(u + i)) { val = val * 10 + (u[i + 2] - 0x90); i += 3; }
            else return 0;
        }
        sl->n = val; sl->have_n = 1;
        return 1;
    }
    return 0;
}

int forge_match(const char *pattern, const char *line, struct forge_slots *sl)
{
    memset(sl, 0, sizeof *sl);
    const char *p = pattern, *s = line;
    while (*p) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (!e) return 0;
            char name[16];
            size_t nl = (size_t)(e - p) - 1;
            if (nl >= sizeof name) return 0;
            memcpy(name, p + 1, nl); name[nl] = '\0';
            p = e + 1;
            /* the literal text up to the next slot decides where the capture ends */
            const char *next = strchr(p, '{');
            size_t ll = next ? (size_t)(next - p) : strlen(p);
            const char *end;
            if (ll == 0) end = s + strlen(s);
            else {
                char lit[FORGE_PAT];
                memcpy(lit, p, ll); lit[ll] = '\0';
                end = strstr(s, lit);
                if (!end) return 0;
            }
            if (!slot_ok(name, s, (size_t)(end - s), sl)) return 0;
            s = end;
        } else {
            if (*s != *p) return 0;
            p++; s++;
        }
    }
    return *s == '\0';
}

int forge_fill(const char *tmpl, const struct forge_slots *sl, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (const char *p = tmpl; *p; ) {
        if (*p != '{') { if (o + 1 >= cap) return 0; out[o++] = *p++; continue; }
        const char *e = strchr(p, '}');
        if (!e) return 0;
        if (e == p + 1) { if (o + 3 >= cap) return 0; out[o++] = '{'; out[o++] = '}'; p = e + 1; continue; }   /* find's {} */
        char name[24], val[FORGE_PATH * 4 + 8];
        size_t nl = (size_t)(e - p) - 1;
        if (nl >= sizeof name) return 0;
        memcpy(name, p + 1, nl); name[nl] = '\0';
        p = e + 1;
        char *bar = strchr(name, '|'), *mode = strchr(name, ':');
        if (bar) *bar++ = '\0';
        if (mode) *mode++ = '\0';
        if (strcmp(name, "path") == 0 || strcmp(name, "path2") == 0) {
            const char *v = name[4] ? sl->path2 : sl->path;
            if (!(name[4] ? sl->have_path2 : sl->have_path)) return 0;
            shell_quote(v, val, sizeof val);
        } else if (strcmp(name, "word") == 0 || strcmp(name, "word2") == 0) {
            const char *v = name[4] ? sl->word2 : sl->word;
            if (!(name[4] ? sl->have_word2 : sl->have_word)) return 0;
            if (mode && strcmp(mode, "sed") == 0) sed_quote(v, val, sizeof val); else shell_quote(v, val, sizeof val);
        } else if (strcmp(name, "ext") == 0) {
            if (!sl->have_ext) return 0;
            snprintf(val, sizeof val, "%s", sl->ext);
        } else if (strcmp(name, "n") == 0) {
            if (sl->have_n) snprintf(val, sizeof val, "%ld", sl->n > 0 ? sl->n : 1);
            else if (bar) snprintf(val, sizeof val, "%s", bar);
            else return 0;
        } else return 0;
        size_t vl = strlen(val);
        if (o + vl + 1 >= cap) return 0;
        memcpy(out + o, val, vl); o += vl;
    }
    out[o] = '\0';
    return 1;
}

void forge_parts(int en, char *out, size_t cap)
{
    snprintf(out, cap, en ? "count (files, folders, lines, characters, words, a word's occurrences, by kind), find (a word in files, files by kind, empty, touched today), "
                           "biggest / newest / oldest, size, copy, move / rename, backup, replace a word, sort / duplicates, first / last lines, frequent words, the shape inside"
                         : "数える(ファイル・フォルダ・行・文字・単語・言葉の回数・種類ごと)、探す(言葉の入ったファイル・種類・空のもの・今日さわったもの)、"
                           "大きい/新しい/古い順、大きさ、コピー、移す/名前を変える、控え(バックアップ)、言葉の置き換え、並べる/重複、先頭/最後の行、よく出る言葉、中の形");
}

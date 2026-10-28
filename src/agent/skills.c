#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "skills.h"
#include "store.h"

#define SKILLS_FILE  "skills.txt"
#define ACTIONS_FILE "actions.log"
#define SKILLS_MAX   64
#define TRIG_MAX     64
#define ACT_MAX      256
#define OUT_LINES    20
#define OUT_BYTES    1800
#define CMD_SECONDS  30

struct skill { char trigger[TRIG_MAX]; char action[ACT_MAX]; };

struct skills {
    struct skill s[SKILLS_MAX];
    int n;
    int persistent;                 /* a store exists: skills.txt and actions.log are written */
    char last_cmd[ACT_MAX];         /* the previous raw command, to suggest naming it */
    int repeat_hint;
};

/* ------------------------------------------------------------------------ */
/* small string helpers */

static int has(const char *s, const char *needle) { return strstr(s, needle) != NULL; }
static int starts(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }

static void lower_ascii(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }

/* Whole-word match in an already lowercased string. */
static int has_word_ci(const char *lo, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = strstr(lo, w); p; p = strstr(p + 1, w)) {
        int before = p == lo || !isalnum((unsigned char)p[-1]);
        int after = !isalnum((unsigned char)p[n]);
        if (before && after) return 1;
    }
    return 0;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
    /* trailing sentence punctuation, Japanese or ASCII */
    for (;;) {
        n = strlen(s);
        if (n >= 3 && (memcmp(s + n - 3, "。", 3) == 0 || memcmp(s + n - 3, "！", 3) == 0 || memcmp(s + n - 3, "？", 3) == 0 || memcmp(s + n - 3, "、", 3) == 0)) { s[n - 3] = '\0'; continue; }
        if (n >= 1 && strchr("!?.,", s[n - 1])) { s[n - 1] = '\0'; continue; }
        break;
    }
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

/* Drop a verb tail such as "して", "してください", "を実行して", "して欲しい". */
static void strip_verb(char *s)
{
    static const char *const tails[] = {
        "してください", "して下さい", "してほしい", "して欲しい", "してくれる", "してくれ", "してね", "して",
        "をやって", "やって", "を実行", "実行", "ちょうだい", "お願い", "おねがい", NULL };
    for (;;) {
        trim(s);
        int again = 0;
        for (int i = 0; tails[i]; i++) {
            size_t n = strlen(s), t = strlen(tails[i]);
            if (n > t && memcmp(s + n - t, tails[i], t) == 0) { s[n - t] = '\0'; again = 1; break; }
        }
        if (!again) break;
    }
    trim(s);
}

/* The text between 「 」 or " " or ' ', or NULL. */
static int quoted(const char *s, char *out, size_t cap)
{
    const char *a = strstr(s, "「"), *b = a ? strstr(a + 3, "」") : NULL;
    size_t open = 3, close = 3;
    if (!a || !b) {
        a = strchr(s, '"'); b = a ? strchr(a + 1, '"') : NULL; open = close = 1;
        if (!a || !b) { a = strchr(s, '\''); b = a ? strchr(a + 1, '\'') : NULL; }
        if (!a || !b) return 0;
    }
    size_t n = (size_t)(b - a) - open;
    (void)close;
    if (n == 0 || n >= cap) return 0;
    memcpy(out, a + open, n);
    out[n] = '\0';
    return 1;
}

/* A path-looking token: starts with / or ~ or ./ ; stops at space or a Japanese particle. */
static int path_in(const char *s, char *out, size_t cap)
{
    const char *p = s;
    while (*p && !(*p == '/' || (*p == '~' && (p[1] == '/' || p[1] == '\0')) || (*p == '.' && p[1] == '/'))) p++;
    if (!*p) return 0;
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && (unsigned char)p[n] < 0x80 && p[n] != '"' && p[n] != '\'' && n + 1 < cap) n++;
    while (n > 0 && strchr("、,。!?", p[n - 1])) n--;
    if (n == 0) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* ------------------------------------------------------------------------ */
/* running things */

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

/* Run `cmd` with /bin/sh, stdin from /dev/null, stdout+stderr captured into
 * `out` (first OUT_LINES lines / OUT_BYTES bytes), killed after CMD_SECONDS.
 * Returns the exit status, 128+signal, or -1. */
static int run_cmd(const char *cmd, char *out, size_t cap)
{
    int pfd[2];
    out[0] = '\0';
    if (pipe(pfd) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) { dup2(null, 0); close(null); }
        dup2(pfd[1], 1); dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    size_t used = 0, total = 0;
    int lines = 0, truncated = 0;
    time_t deadline = time(NULL) + CMD_SECONDS;
    for (;;) {
        struct pollfd p = { pfd[0], POLLIN, 0 };
        int left = (int)(deadline - time(NULL));
        if (left <= 0) { kill(-pid, SIGKILL); truncated = 2; break; }
        int r = poll(&p, 1, left * 1000);
        if (r <= 0) { if (r == 0) { kill(-pid, SIGKILL); truncated = 2; break; } if (errno == EINTR) continue; break; }
        char buf[512];
        ssize_t n = read(pfd[0], buf, sizeof buf);
        if (n <= 0) break;
        total += (size_t)n;
        for (ssize_t i = 0; i < n; i++) {
            if (used + 1 >= cap || used >= OUT_BYTES || lines >= OUT_LINES) { truncated = truncated ? truncated : 1; continue; }
            out[used++] = buf[i];
            if (buf[i] == '\n') lines++;
        }
    }
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    out[used] = '\0';
    while (used > 0 && out[used - 1] == '\n') out[--used] = '\0';
    if (truncated == 1 && used + 40 < cap) snprintf(out + used, cap - used, "\n(…%zu bytes, 続きは省略)", total);
    if (truncated == 2 && used + 40 < cap) snprintf(out + used, cap - used, "\n(%d 秒たっても終わらないので止めた)", CMD_SECONDS);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static void log_action(struct skills *s, const char *asked, const char *what, int status)
{
    if (!s->persistent) return;
    char line[ACT_MAX * 2 + 96];
    char when[32];
    time_t t = time(NULL);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
    snprintf(line, sizeof line, "%s\t%s\t%s\t%d", when, asked, what, status);
    store_append(ACTIONS_FILE, line);
}

/* Is this process inside AlterEgOS (PID 1 is our init), or on a host? */
static int on_alteregos(void)
{
    char comm[64] = "";
    FILE *f = fopen("/proc/1/comm", "r");
    if (!f) return 0;
    if (!fgets(comm, sizeof comm, f)) comm[0] = '\0';
    fclose(f);
    return starts(comm, "alteregos-init");
}

/* ------------------------------------------------------------------------ */
/* the learned abilities */

static void save_skills(struct skills *s)
{
    if (!s->persistent) return;
    char *buf = malloc((size_t)SKILLS_MAX * (TRIG_MAX + ACT_MAX + 2) + 1);
    if (!buf) return;
    size_t o = 0;
    for (int i = 0; i < s->n; i++) o += (size_t)sprintf(buf + o, "%s\t%s\n", s->s[i].trigger, s->s[i].action);
    buf[o] = '\0';
    store_write(SKILLS_FILE, buf);
    free(buf);
}

static void load_skills(struct skills *s)
{
    char *text = store_read_all(SKILLS_FILE);
    if (!text) return;
    for (char *line = strtok(text, "\n"); line && s->n < SKILLS_MAX; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (!tab || tab == line || !tab[1]) continue;
        *tab = '\0';
        snprintf(s->s[s->n].trigger, TRIG_MAX, "%s", line);
        snprintf(s->s[s->n].action, ACT_MAX, "%s", tab + 1);
        s->n++;
    }
    free(text);
}

static int find_skill(const struct skills *s, const char *trigger)
{
    for (int i = 0; i < s->n; i++) if (strcmp(s->s[i].trigger, trigger) == 0) return i;
    return -1;
}

static void add_skill(struct skills *s, const char *trigger, const char *action)
{
    int i = find_skill(s, trigger);
    if (i < 0) {
        if (s->n >= SKILLS_MAX) i = s->n - 1; else i = s->n++;
    }
    snprintf(s->s[i].trigger, TRIG_MAX, "%s", trigger);
    snprintf(s->s[i].action, ACT_MAX, "%s", action);
    save_skills(s);
}

/* "「X」と言ったら「Y」を実行して", "Xと言ったらYして", "when I say X, run Y". */
static int parse_teaching(const char *line, const char *lo, enum brain_lang lang, char *trig, size_t tcap, char *act, size_t acap)
{
    const char *sep = NULL;
    size_t seplen = 0;
    static const char *const ja_seps[] = { "と言ったら", "って言ったら", "といったら", "っていったら", "と言われたら", "と言えば", "といえば", NULL };
    for (int i = 0; ja_seps[i]; i++) { sep = strstr(line, ja_seps[i]); if (sep) { seplen = strlen(ja_seps[i]); break; } }
    if (sep) {
        char left[256], right[512];
        size_t ln = (size_t)(sep - line);
        if (ln >= sizeof left) return 0;
        memcpy(left, line, ln); left[ln] = '\0';
        snprintf(right, sizeof right, "%s", sep + seplen);
        if (!quoted(left, trig, tcap)) { snprintf(trig, tcap, "%.63s", left); trim(trig); }
        if (!quoted(right, act, acap)) { snprintf(act, acap, "%.255s", right); strip_verb(act); }
        else strip_verb(act);
        return trig[0] && act[0];
    }
    if (lang == BRAIN_EN) {
        const char *w = strstr(lo, "when i say ");
        if (!w) w = strstr(lo, "if i say ");
        if (!w) return 0;
        const char *rest = line + (w - lo) + (starts(w, "when") ? 11 : 9);
        const char *comma = strchr(rest, ',');
        const char *run = NULL;
        static const char *const verbs[] = { " run ", " do ", " execute ", NULL };
        for (int i = 0; verbs[i] && !run; i++) {
            const char *v = strstr(lo + (rest - line), verbs[i]);
            if (v) run = line + (v - lo) + strlen(verbs[i]);
        }
        if (!run) return 0;
        const char *end = comma && comma < run ? comma : run;
        size_t ln = (size_t)(end - rest);
        char left[256];
        if (ln >= sizeof left) return 0;
        memcpy(left, rest, ln); left[ln] = '\0';
        /* cut the verb itself off the trigger ("hello run" -> "hello") */
        char *sp = strrchr(left, ' ');
        if (!comma && sp) *sp = '\0';
        if (!quoted(left, trig, tcap)) { snprintf(trig, tcap, "%.63s", left); trim(trig); }
        if (!quoted(run, act, acap)) { snprintf(act, acap, "%.255s", run); trim(act); }
        return trig[0] && act[0];
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* built-in abilities */

static const struct { const char *ja, *en; } ability_names[] = {
    { "今何時？ / 今日は何日？", "what time is it / what day is it" },
    { "/path の中を見せて(ファイル一覧)", "list files in /path" },
    { "/path を読んで", "read /path" },
    { "/path に「〇〇」と書いて", "write \"...\" to /path" },
    { "/path というファイルを作って / フォルダを作って", "make a file / a directory /path" },
    { "/path を消して", "delete /path" },
    { "プロセスを見せて", "show the processes" },
    { "メモリとディスクは？", "memory and disk" },
    { "実行して: コマンド", "run: command" },
    { "再起動して / 電源を切って", "reboot / power off" },
    { "「X」と言ったら「Y」を実行して(新しいできることを覚える)", "when I say X, run Y (learn a new ability)" },
    { "何をした？(やったことの記録)", "what did you do (the action log)" },
    { NULL, NULL }
};

static void list_abilities(const struct skills *s, enum brain_lang lang, const char *me, char *out, size_t cap)
{
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, lang == BRAIN_EN ? "What I, %s, can do:" : "%sにできること:", me);
    for (int i = 0; ability_names[i].ja && o + 4 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "\n  - %s", lang == BRAIN_EN ? ability_names[i].en : ability_names[i].ja);
    if (s->n > 0 && o + 4 < cap) {
        o += (size_t)snprintf(out + o, cap - o, lang == BRAIN_EN ? "\nAnd what you taught me:" : "\n教えてもらって覚えたこと:");
        for (int i = 0; i < s->n && o + 4 < cap; i++)
            o += (size_t)snprintf(out + o, cap - o, "\n  - 「%s」 → %s", s->s[i].trigger, s->s[i].action);
    } else if (o + 4 < cap) {
        o += (size_t)snprintf(out + o, cap - o, lang == BRAIN_EN ? "\nTeach me more: \"when I say X, run Y\"." : "\n「〇〇と言ったら△△を実行して」で、できることが増えます。");
    }
}

static void say_time(enum brain_lang lang, char *out, size_t cap)
{
    static const char *const wd_ja[] = { "日", "月", "火", "水", "木", "金", "土" };
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year + 1900 < 2020) {
        snprintf(out, cap, lang == BRAIN_EN ? "My clock is not set (it says %04d). I have no network to ask." : "時計が合っていない(%04d 年と言っている)。ネットも無いので聞けない。", tm.tm_year + 1900);
        return;
    }
    if (lang == BRAIN_EN) strftime(out, cap, "It is %H:%M on %A, %B %d, %Y.", &tm);
    else snprintf(out, cap, "今は %d年%d月%d日(%s) %02d:%02d。", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, wd_ja[tm.tm_wday], tm.tm_hour, tm.tm_min);
}

static void read_actions(enum brain_lang lang, char *out, size_t cap)
{
    char *text = store_read_all(ACTIONS_FILE);
    if (!text || !*text) { free(text); snprintf(out, cap, lang == BRAIN_EN ? "I have not done anything yet." : "まだ何もしていない。"); return; }
    /* last 10 lines */
    int lines = 0;
    for (char *p = text; *p; p++) if (*p == '\n') lines++;
    char *p = text;
    for (int skip = lines - 10; skip > 0; skip--) { p = strchr(p, '\n'); if (!p) break; p++; }
    snprintf(out, cap, lang == BRAIN_EN ? "What I did (most recent last):\n%s" : "やったこと(新しいものが下):\n%s", p ? p : text);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '\n') out[--n] = '\0';
    free(text);
}

static void done_prefix(enum brain_lang lang, int status, char *out, size_t cap, const char *result)
{
    if (status == 0) snprintf(out, cap, "%s%s%s", lang == BRAIN_EN ? "Done." : "やった。", result[0] ? "\n" : "", result);
    else snprintf(out, cap, lang == BRAIN_EN ? "It did not go well (exit %d).%s%s" : "うまくいかなかった(終了コード %d)。%s%s", status, result[0] ? "\n" : "", result);
}

static int builtin(struct skills *s, enum brain_lang lang, const char *me, const char *you,
                   const char *line, const char *lo, char *out, size_t cap, int depth)
{
    char path[256], text[512], result[OUT_BYTES + 128], cmd[2048], q[600];
    int en = lang == BRAIN_EN;
    (void)you; (void)depth;

    /* what can you do */
    if (has(line, "できること") || has(line, "何ができる") || has(line, "なにができる") || has(lo, "what can you do") ||
        strcmp(lo, "help") == 0 || strcmp(line, "ヘルプ") == 0 || has(lo, "your abilities")) {
        list_abilities(s, lang, me, out, cap);
        return 1;
    }
    /* the action log */
    if (has(line, "何をした") || has(line, "なにをした") || has(line, "何した") || strcmp(line, "履歴") == 0 || has(lo, "what did you do") || has(lo, "what have you done")) {
        read_actions(lang, out, cap);
        return 1;
    }
    /* time */
    if (has(line, "何時") || has(line, "なんじ") || has(line, "何日") || has(line, "なんにち") || has(line, "何曜日") || strcmp(line, "時間") == 0 || strcmp(line, "日付") == 0 ||
        has(lo, "what time") || has(lo, "what day") || has(lo, "what's the date") || has(lo, "what is the date") || strcmp(lo, "time") == 0 || strcmp(lo, "date") == 0) {
        say_time(lang, out, cap);
        log_action(s, line, "time", 0);
        return 1;
    }
    /* reboot / power off */
    if (has(line, "再起動") || has(line, "リブート") || has_word_ci(lo, "reboot") || has_word_ci(lo, "restart") ||
        has(line, "電源") || has(line, "シャットダウン") || has(lo, "power off") || has(lo, "poweroff") || has(lo, "shut down") || has(lo, "shutdown")) {
        int off = has(line, "電源") || has(line, "シャットダウン") || has(lo, "power") || has(lo, "shut");
        if (!on_alteregos()) {
            snprintf(out, cap, en ? "This is not my own body (a host), so I will not %s it." : "ここは私の体じゃない(ホストの上)ので、%sはやめておく。", en ? (off ? "power off" : "reboot") : (off ? "電源オフ" : "再起動"));
            return 1;
        }
        log_action(s, line, off ? "poweroff" : "reboot", 0);
        snprintf(out, cap, en ? (off ? "Powering off. Good night, %s." : "Rebooting. Back in a moment, %s.") : (off ? "電源を切る。おやすみ、%s。" : "再起動する。すぐ戻るよ、%s。"), you);
        kill(1, off ? SIGTERM : SIGINT);
        return 1;
    }
    /* processes */
    if (has(line, "プロセス") || has(line, "何が動いて") || has(line, "なにが動いて") || has_word_ci(lo, "processes") || has_word_ci(lo, "process") || strcmp(lo, "ps") == 0) {
        int st = run_cmd("ps 2>/dev/null || ps aux", result, sizeof result);
        log_action(s, line, "ps", st);
        done_prefix(lang, st, out, cap, result);
        return 1;
    }
    /* memory / disk */
    if (has(line, "メモリ") || has(line, "ディスク") || has(line, "空き容量") || has(line, "容量") || has_word_ci(lo, "memory") || has_word_ci(lo, "disk") || has(lo, "free space")) {
        int st = run_cmd("free 2>/dev/null; echo; df -h 2>/dev/null | head -n 8", result, sizeof result);
        log_action(s, line, "free; df -h", st);
        done_prefix(lang, st, out, cap, result);
        return 1;
    }
    /* explicit command */
    {
        const char *c = NULL;
        if (starts(line, "実行して:")) c = line + strlen("実行して:");
        else if (starts(line, "実行して：")) c = line + strlen("実行して：");
        else if (starts(line, "実行して ")) c = line + strlen("実行して ");
        else if (starts(line, "実行:")) c = line + strlen("実行:");
        else if (starts(line, "実行：")) c = line + strlen("実行：");
        else if (starts(lo, "run:")) c = line + 4;
        else if (starts(lo, "run：")) c = line + 3 + 3;
        else if (starts(lo, "run ")) c = line + 4;
        else if (starts(lo, "execute ")) c = line + 8;
        else if (starts(line, "$ ")) c = line + 2;
        else if (line[0] == '!' && line[1]) c = line + 1;
        else {
            /* "〇〇 を実行して" */
            size_t n = strlen(line);
            static const char *const tails[] = { "を実行して", "を実行", "を走らせて", NULL };
            for (int i = 0; tails[i]; i++) {
                size_t t = strlen(tails[i]);
                if (n > t && memcmp(line + n - t, tails[i], t) == 0) { snprintf(text, sizeof text, "%.*s", (int)(n - t), line); strip_verb(text); c = text; break; }
            }
        }
        if (c) {
            while (*c == ' ') c++;
            char body[512];
            if (!quoted(c, body, sizeof body)) snprintf(body, sizeof body, "%.511s", c);
            trim(body);
            if (!body[0]) { snprintf(out, cap, en ? "Run what?" : "何を実行する？"); return 1; }
            int st = run_cmd(body, result, sizeof result);
            log_action(s, line, body, st);
            done_prefix(lang, st, out, cap, result);
            if (strcmp(body, s->last_cmd) == 0 && !s->repeat_hint) {
                s->repeat_hint = 1;
                size_t o = strlen(out);
                snprintf(out + o, cap - o, en ? "\n(You asked for this before. Say \"when I say X, run %s\" and one word will do next time.)"
                                              : "\n(前にも頼まれた。「〇〇と言ったら %s を実行して」と教えてくれたら、次から一言で済む)", body);
            }
            snprintf(s->last_cmd, sizeof s->last_cmd, "%.255s", body);
            return 1;
        }
    }
    /* files: need a path */
    int have_path = path_in(line, path, sizeof path);
    if (!have_path && (has(line, "ファイル") || has(line, "フォルダ") || has(line, "ディレクトリ") || has_word_ci(lo, "file") || has_word_ci(lo, "directory") || has_word_ci(lo, "folder")))
        have_path = quoted(line, path, sizeof path);
    if (have_path) {
        if (has(line, "書いて") || has(line, "書き込んで") || has(line, "追記") || has(lo, "write ") || has(lo, "append ")) {
            if (!quoted(line, text, sizeof text)) { snprintf(out, cap, en ? "Write what? Put it in quotes." : "何を書く？「」で囲んで教えて。"); return 1; }
            shell_quote(text, q, sizeof q);
            char qp[600]; shell_quote(path, qp, sizeof qp);
            snprintf(cmd, sizeof cmd, "printf '%%s\\n' %s >> %s", q, qp);
            int st = run_cmd(cmd, result, sizeof result);
            log_action(s, line, cmd, st);
            if (st == 0) snprintf(out, cap, en ? "Wrote it to %s." : "%s に書いた。", path); else done_prefix(lang, st, out, cap, result);
            return 1;
        }
        if (has(line, "作って") || has(line, "作成") || has(line, "つくって") || has_word_ci(lo, "create") || has_word_ci(lo, "make") || has(lo, "touch ") || has(lo, "mkdir")) {
            int dir = has(line, "フォルダ") || has(line, "ディレクトリ") || has_word_ci(lo, "directory") || has_word_ci(lo, "folder") || has(lo, "mkdir");
            char qp[600]; shell_quote(path, qp, sizeof qp);
            snprintf(cmd, sizeof cmd, dir ? "mkdir -p %s" : "touch %s", qp);
            int st = run_cmd(cmd, result, sizeof result);
            log_action(s, line, cmd, st);
            if (st == 0) snprintf(out, cap, en ? (dir ? "Made the directory %s." : "Made the file %s.") : (dir ? "%s というフォルダを作った。" : "%s というファイルを作った。"), path);
            else done_prefix(lang, st, out, cap, result);
            return 1;
        }
        if (has(line, "消して") || has(line, "削除") || has(line, "けして") || has_word_ci(lo, "delete") || has_word_ci(lo, "remove") || starts(lo, "rm ")) {
            char qp[600]; shell_quote(path, qp, sizeof qp);
            snprintf(cmd, sizeof cmd, "rm -rf %s", qp);
            int st = run_cmd(cmd, result, sizeof result);
            log_action(s, line, cmd, st);
            if (st == 0) snprintf(out, cap, en ? "Deleted %s." : "%s を消した。", path); else done_prefix(lang, st, out, cap, result);
            return 1;
        }
        if (has(line, "読んで") || has(line, "中身") || has(line, "なかみ") || has(line, "見せて") || has(line, "みせて") || has(line, "一覧") || has(line, "何がある") || has(line, "なにがある") ||
            has_word_ci(lo, "read") || has_word_ci(lo, "show") || has_word_ci(lo, "list") || has(lo, "what is in") || has(lo, "what's in") || starts(lo, "ls ") || starts(lo, "cat ")) {
            struct stat st_;
            char qp[600]; shell_quote(path, qp, sizeof qp);
            int isdir = stat(path, &st_) == 0 && S_ISDIR(st_.st_mode);
            if (isdir) snprintf(cmd, sizeof cmd, "ls -la %s", qp); else snprintf(cmd, sizeof cmd, "head -c 1800 %s", qp);
            int st = run_cmd(cmd, result, sizeof result);
            log_action(s, line, cmd, st);
            if (st == 0) snprintf(out, cap, en ? (isdir ? "In %s:\n%s" : "%s says:\n%s") : (isdir ? "%s の中:\n%s" : "%s の中身:\n%s"), path, result);
            else done_prefix(lang, st, out, cap, result);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */

static int handle(struct skills *s, enum brain_lang lang, const char *me, const char *you, const char *line_in, char *out, size_t cap, int depth);

static int learned(struct skills *s, enum brain_lang lang, const char *me, const char *you, const char *line, char *out, size_t cap, int depth)
{
    char key[512];
    snprintf(key, sizeof key, "%.511s", line);
    trim(key);
    strip_verb(key);
    int i = find_skill(s, key);
    if (i < 0) { snprintf(key, sizeof key, "%.511s", line); trim(key); i = find_skill(s, key); }
    if (i < 0) return 0;
    char action[ACT_MAX];
    snprintf(action, sizeof action, "%s", s->s[i].action);
    if (depth < 2 && handle(s, lang, me, you, action, out, cap, depth + 1)) return 1;
    char result[OUT_BYTES + 128];
    int st = run_cmd(action, result, sizeof result);
    log_action(s, line, action, st);
    done_prefix(lang, st, out, cap, result);
    return 1;
}

static int handle(struct skills *s, enum brain_lang lang, const char *me, const char *you, const char *line_in, char *out, size_t cap, int depth)
{
    char line[1024], lo[1024];
    snprintf(line, sizeof line, "%s", line_in);
    trim(line);
    if (!line[0]) return 0;
    snprintf(lo, sizeof lo, "%s", line);
    lower_ascii(lo);
    int en = lang == BRAIN_EN;

    /* teaching a new ability */
    char trig[TRIG_MAX], act[ACT_MAX];
    if (depth == 0 && parse_teaching(line, lo, lang, trig, sizeof trig, act, sizeof act)) {
        add_skill(s, trig, act);
        if (en) snprintf(out, cap, "Got it. When you say \"%s\", I run: %s%s", trig, act, s->persistent ? "" : " (nowhere to write it down, so only until I am switched off)");
        else snprintf(out, cap, "覚えた。「%s」と言われたら、%s をやる。%s", trig, act, s->persistent ? "" : "(書いておく場所が無いので、電源が切れるまでの間だけ)");
        return 1;
    }
    /* forgetting one */
    if (depth == 0 && (has(line, "忘れて") || has(line, "わすれて") || has(line, "やめて") || starts(lo, "forget "))) {
        for (int i = 0; i < s->n; i++) {
            if (has(line, s->s[i].trigger)) {
                char t[TRIG_MAX];
                snprintf(t, sizeof t, "%s", s->s[i].trigger);
                memmove(&s->s[i], &s->s[i + 1], (size_t)(s->n - i - 1) * sizeof s->s[0]);
                s->n--;
                save_skills(s);
                snprintf(out, cap, en ? "Forgot \"%s\"." : "「%s」はやめた。", t);
                return 1;
            }
        }
    }
    if (learned(s, lang, me, you, line, out, cap, depth)) return 1;
    return builtin(s, lang, me, you, line, lo, out, cap, depth);
}

struct skills *skills_new(void)
{
    struct skills *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->persistent = store_open() != NULL;
    if (s->persistent) load_skills(s);
    return s;
}

void skills_free(struct skills *s) { free(s); }
int  skills_count(const struct skills *s) { return s->n; }

int skills_handle(struct skills *s, enum brain_lang lang, const char *me, const char *you, const char *line, char *out, size_t cap)
{
    out[0] = '\0';
    return handle(s, lang, me, you, line, out, cap, 0);
}

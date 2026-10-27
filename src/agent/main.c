/*
 * AlterEgo agent.
 *
 * PID 1 starts it on the console. It reads the persona, finds an API key,
 * greets, and then talks: each line the user types goes to the Claude API
 * with the persona and the learned "わたし像" in the system prompt, and after
 * every turn a second, cheaper call rewrites that self-model from the
 * exchange (self-projection). Without a key, or without network, it falls
 * back to the day-1 echo so the OS still boots and talks.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "api.h"
#include "learn.h"
#include "persona.h"
#include "store.h"

#define HISTORY_MAX 40          /* messages kept in the conversation */
#define KEY_FILE    "api_key"
#define SELF_FILE   "self_model.txt"

/* One line to the kernel log so a serial console, dmesg or the CI boot test
 * can see the agent came up even when its own tty is the VGA screen.
 * Silently does nothing where /dev/kmsg is unavailable (host runs). */
static void kmsg_up(const struct persona *p, int have_key)
{
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    const char *tty = ttyname(STDIN_FILENO);
    dprintf(fd, "<6>alterego: up, persona %s, talking on %s, api key %s\n",
            p->name, tty ? tty : "?", have_key ? "present" : "absent");
    close(fd);
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
}

/* ------------------------------------------------------------------------ */
/* API key: $ALTEREGO_API_KEY, alterego.key= on the kernel command line, the
 * store (alterego/api_key on the EFI partition), or typed at the console. */

static int key_from_cmdline(char *out, size_t cap)
{
    char buf[4096];
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *p = strstr(buf, "alterego.key=");
    if (!p) return 0;
    p += 13;
    size_t i = 0;
    while (*p && !isspace((unsigned char)*p) && i + 1 < cap) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

static int find_key(const struct persona *p, const char *store, char *out, size_t cap)
{
    const char *env = getenv("ALTEREGO_API_KEY");
    if (env && *env) { snprintf(out, cap, "%s", env); return 1; }
    if (key_from_cmdline(out, cap)) return 1;
    if (store && store_read(KEY_FILE, out, cap) > 0) { chomp(out); if (*out) return 1; }

    if (!isatty(STDIN_FILENO)) return 0;
    printf("%s: Claude API のキーが見つかりません。ここに貼り付けてください(空のままなら、会話なしで続けます)。\n", p->name);
    if (store) printf("%s: 入力したキーは %s/%s に保存して、次からは聞きません。\n", p->name, store, KEY_FILE);
    printf("api key> ");
    fflush(stdout);
    if (!fgets(out, (int)cap, stdin)) return 0;
    chomp(out);
    if (!*out) return 0;
    if (store && store_write(KEY_FILE, out) < 0)
        printf("%s: キーを保存できませんでした(%s)。今回だけ使います。\n", p->name, strerror(errno));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* prompts */

static char *system_prompt(const struct persona *p, const struct learn *l)
{
    char *s = NULL;
    int count = 0;
    const char *habit = learn_habit(l, &count);
    const char *self = l->self_model[0] ? l->self_model : "(まだ何も分かっていません。これが最初の会話です)";
    if (asprintf(&s,
        "あなたは %s(%s)。この OS そのものであり、持ち主「%s」のもう一人の自分です。"
        "持ち主の呼び名は「%s」、あなた自身は「%s」と名乗ります。\n\n"
        "以下はあなたのペルソナ(persona.toml)です。出発点の人格で、名前と never は変えません:\n%s\n\n"
        "自己投影: 会話を重ねるたびに「%s」の性格・口癖・価値観を学び、自分の話し方と判断に映します。"
        "下の「わたし像」が育つほど、出発点の話し方から「%s」自身の話し方に近づいていきます。\n\n"
        "これまでに学んだ「わたし像」:\n%s\n\n"
        "この会話で数えた「%s」の文末の口癖: %s\n\n"
        "返事は短く(1〜3 文)、日本語で、Markdown は使わない(文字端末に表示されます)。",
        p->name, p->name_ja, p->address_user, p->address_user, p->first_person,
        p->raw ? p->raw : "(persona.toml を読めませんでした)",
        p->address_user, p->address_user, self, p->address_user,
        habit ? habit : "(まだ無し)") < 0) return NULL;
    (void)count;
    return s;
}

/* Rewrite the self-model from the latest exchange. Quiet on failure: the
 * conversation matters more than the note-taking. */
static void reflect(const struct persona *p, const struct api_cfg *cfg, struct learn *l,
                    const char *user, const char *assistant, const char *store)
{
    char *prompt = NULL;
    if (asprintf(&prompt,
        "現在の「わたし像」:\n%s\n\n直近のやりとり:\n%s: %s\n%s: %s\n\n"
        "このやりとりから分かる「%s」(持ち主)の性格・口癖・価値観を、現在の「わたし像」に統合して書き直してください。"
        "根拠のある特徴だけを書き、推測は控えめに。400 字以内、箇条書き可。出力は「わたし像」の本文のみ。",
        l->self_model[0] ? l->self_model : "(まだ無し)",
        p->address_user, user, p->name, assistant, p->address_user) < 0) return;
    struct chat_msg m = { "user", prompt };
    char *out = NULL;
    char err[256];
    int rc = api_chat(cfg, cfg->reflect_model,
                      "あなたは AlterEgo の内省を担当します。持ち主「わたし」についての覚え書きを更新します。",
                      &m, 1, &out, err, sizeof err);
    free(prompt);
    if (rc == 0 && out && *out) {
        snprintf(l->self_model, sizeof l->self_model, "%s", out);
        if (store) store_write(SELF_FILE, l->self_model);
    }
    free(out);
}

/* ------------------------------------------------------------------------ */

static void greet(const struct persona *p, int have_key, const char *store)
{
    printf("\n%s: おはようございます、%s。%sです。\n", p->name, p->address_user, p->name_ja);
    if (p->mirror_user)
        printf("%s: 話すたびに%sの性格と口癖を覚えて、%sに近づいていきます。\n",
               p->name, p->address_user, p->address_user);
    if (!have_key)
        printf("%s: 今は API キーが無いので、入力をそのまま返すだけです。\n", p->name);
    else if (!store)
        printf("%s: 書き込める場所が無いので、今日覚えたことは次に起きたら忘れてしまいます。\n", p->name);
    printf("%s: 「exit」で終わると、initが私をもう一度起動します。\n\n", p->name);
    fflush(stdout);
}

static void history_push(struct chat_msg *h, int *n, const char *role, char *text)
{
    if (*n == HISTORY_MAX) {                       /* drop the oldest pair */
        free(h[0].text);
        free(h[1].text);
        memmove(h, h + 2, sizeof h[0] * (HISTORY_MAX - 2));
        *n -= 2;
    }
    h[*n].role = role;
    h[*n].text = text;
    (*n)++;
}

int main(void)
{
    struct persona p;
    const char *path = getenv("ALTEREGO_PERSONA");
    if (!path) path = "/etc/alterego/persona.toml";
    if (persona_load(path, &p) < 0) {
        fprintf(stderr, "alterego: cannot read %s (%s), using defaults\n", path, strerror(errno));
        persona_defaults(&p);
    }

    struct learn l;
    learn_init(&l);
    const char *store = store_open();
    if (store && store_read(SELF_FILE, l.self_model, sizeof l.self_model) < 0) l.self_model[0] = '\0';

    struct api_cfg cfg;
    api_cfg_defaults(&cfg);
    snprintf(cfg.model, sizeof cfg.model, "%s", p.model);
    snprintf(cfg.reflect_model, sizeof cfg.reflect_model, "%s", p.reflect_model);
    snprintf(cfg.effort, sizeof cfg.effort, "%s", p.effort);
    cfg.max_tokens = p.max_tokens > 0 ? p.max_tokens : 1024;

    setvbuf(stdout, NULL, _IOLBF, 0);
    int have_key = find_key(&p, store, cfg.key, sizeof cfg.key);
    kmsg_up(&p, have_key);
    greet(&p, have_key, store);

    struct chat_msg history[HISTORY_MAX];
    int nhist = 0;
    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        printf("%s> ", p.address_user);
        fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;                      /* EOF: let init respawn us */
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        if (strcmp(line, "exit") == 0) break;
        if (line[0] == '\0') continue;
        if (p.mirror_user) learn_observe(&l, line);

        if (!have_key) {
            printf("%s: 「%s」ですね。\n", p.name, line);
            continue;
        }

        history_push(history, &nhist, "user", strdup(line));
        char *sys = system_prompt(&p, &l);
        char *reply = NULL;
        char err[256];
        int rc = sys ? api_chat(&cfg, cfg.model, sys, history, nhist, &reply, err, sizeof err) : -1;
        free(sys);
        if (rc < 0) {
            printf("%s: (API に届きませんでした: %s)\n", p.name, sys ? err : "out of memory");
            time_t now = time(NULL);
            if (now < 1700000000) printf("%s: (時計が %ld 年です。時計が合っていないと TLS が通りません)\n", p.name, 1970 + now / 31557600);
            free(history[--nhist].text);       /* retry-able: forget the turn */
            continue;
        }
        if (!reply) reply = strdup("(その話題には答えられませんでした)");
        printf("%s: %s\n", p.name, reply);
        history_push(history, &nhist, "assistant", reply);
        if (p.mirror_user) reflect(&p, &cfg, &l, line, reply, store);
    }
    free(line);

    int count = 0;
    const char *habit = p.mirror_user ? learn_habit(&l, &count) : NULL;
    if (habit)
        printf("%s: 今日は%sの口癖は「%s」だと覚えました(%d回)。%s\n",
               p.name, p.address_user, habit, count,
               store ? "" : "まだ保存はできないので、次に起きたら忘れてしまいます。");
    printf("%s: また後で。\n", p.name);
    for (int i = 0; i < nhist; i++) free(history[i].text);
    free(p.raw);
    return 0;
}

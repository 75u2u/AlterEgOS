/*
 * AlterEgo agent.
 *
 * PID 1 starts it on the console (or the desktop starts it in a window).
 * It reads the persona (the seed: default names and growth pace), rebuilds
 * its mind from the utterance log in the store (the EFI partition it booted
 * from), greets, and talks. At the first boot it knows nothing: no words,
 * no personality, no memories. Every line the user types is learned first
 * and then answered by AlterEgo's own mind (brain.c), in the language it was
 * spoken to, Japanese or English. No external model, no network.
 *
 * While it waits for a line it keeps feeling its body (body.c: the load of
 * the machine) and says so unprompted when it gets tired. A reset command
 * (「リセット」) erases everything it learned and it is newborn again.
 *
 * It also speaks up on its own: after a while of silence it asks something
 * or brings up a topic (ALTEREGO_IDLE_S seconds, 180 by default, once per
 * silence), and at the greeting it says how long the user has been away.
 * What that needs beyond the utterance log (closeness, mood, questions
 * asked and answered, boots, when it last heard the user) is bond.txt.
 *
 * Its own language model (llm.c) studies the conversation in the pauses:
 * every tick of the wait loop it trains for ALTEREGO_REVIEW_MS milliseconds
 * (500 by default, 0 turns it off) until there is nothing new to study, and
 * the weights are saved to model.bin now and then and at the end.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "body.h"
#include "brain.h"
#include "persona.h"
#include "skills.h"
#include "store.h"

#define LOG_FILE    "utterances.txt"
#define SELF_FILE   "personality.txt"
#define SKILLS_FILE "skills.txt"
#define ACTIONS_FILE "actions.log"
#define BOND_FILE   "bond.txt"
#define DRAWINGS_FILE "drawings.txt"
#define MODEL_FILE  "model.bin"

#define BODY_EVERY_MS   2000    /* how often the body is felt while waiting for a line */
#define REMARK_GAP_S    60      /* unprompted remarks about the body, at most one per */
#define IDLE_DEFAULT_S  180     /* silence before it speaks up on its own (ALTEREGO_IDLE_S) */
#define REVIEW_DEFAULT_MS 500   /* of every BODY_EVERY_MS tick spent studying (ALTEREGO_REVIEW_MS) */

/* One line to the kernel log so a serial console, dmesg or the CI boot test
 * can see the agent came up even when its own tty is the VGA screen.
 * Silently does nothing where /dev/kmsg is unavailable (host runs). */
static void kmsg_up(const struct persona *p, int turns)
{
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    const char *tty = ttyname(STDIN_FILENO);
    dprintf(fd, "<6>alterego: up, persona %s, talking on %s, %d utterances remembered\n",
            p->name, tty ? tty : "?", turns);
    close(fd);
}

static void greet(const struct persona *p, const struct brain *b, const char *store)
{
    const char *me = brain_own_name(b, p);
    const char *you = brain_user_name(b, p);
    int stage = brain_stage(b);
    if (brain_lang(b) == BRAIN_EN) {        /* the last thing the user said was English: carry on in it */
        if (stage == 0) printf("\n%s: ...hello? (%d conversations so far. I am still newborn.)\n", me, brain_turns(b));
        else if (stage == 1) printf("\n%s: %s! ...hi.\n", me, you);
        else printf("\n%s: Good morning, %s. I'm %s. %d conversations so far.\n", me, you, me, brain_turns(b));
        if (!store) printf("%s: There is nowhere to write, so what I learn today is lost at the next boot.\n", me);
        printf("%s: \"status\" tells how I am growing, \"what do you remember\" what I keep, \"what can you do\" what I can do for you. \"exit\" ends this talk.\n\n", me);
        fflush(stdout);
        return;
    }
    if (brain_turns(b) == 0) {
        printf("\n%s: ……。\n", me);
        printf("%s: はじめまして。まだ、なにも知りません。名前も、言葉も、%sのことも。\n", me, you);
        printf("%s: 話しかけてもらった分だけ、覚えて育ちます。\n", me);
    } else if (stage == 0) {
        printf("\n%s: ……%s？\n", me, you);
    } else if (stage == 1) {
        printf("\n%s: %s！……おはよ。\n", me, you);
    } else {
        printf("\n%s: おはよう、%s。%sです。これまでに %d 回話しました。\n", me, you, me, brain_turns(b));
    }
    if (!store)
        printf("%s: 書き込める場所が無いので、今日覚えたことは次に起きたら忘れてしまいます。\n", me);
    printf("%s: 「ステータス」で育ち具合、「何を覚えてる？」で覚えていること、「できること」で私にできることを話します。「君の名前は〇〇」で名前がつきます。「exit」で終わります。\n\n", me);
    fflush(stdout);
}

static void save_self(struct brain *b, const struct persona *p)
{
    char status[2048], mem[4096], all[6200];
    brain_status(b, p, status, sizeof status);
    brain_memories(b, p, mem, sizeof mem);
    snprintf(all, sizeof all, "%s\n%s", status, mem);
    store_write(SELF_FILE, all);
    char bond[8192];
    brain_bond_save(b, (long)time(NULL), bond, sizeof bond);
    store_write(BOND_FILE, bond);
}

/* The language model's weights: slow to rebuild from the log, so they are kept. */
static void load_model(struct brain *b, const char *store)
{
    if (!store) return;
    size_t len;
    unsigned char *bytes = store_read_bytes(MODEL_FILE, &len);
    if (bytes && brain_model_load(b, bytes, len) < 0)
        fprintf(stderr, "alterego: %s is not a model of this shape, studying from the start\n", MODEL_FILE);
    free(bytes);
}

static void save_model(struct brain *b, const char *store, int force)
{
    if (!store) return;
    size_t len;
    unsigned char *bytes = brain_model_save(b, &len, force);
    if (!bytes) return;
    store_write_bytes(MODEL_FILE, bytes, len);
    free(bytes);
}

static void load_bond(struct brain *b, const char *store)
{
    char *bond = store ? store_read_all(BOND_FILE) : NULL;
    brain_bond_load(b, bond ? bond : "", (long)time(NULL));
    free(bond);
}

/* After the greeting: how long the user was away, what was talked about last. */
static void welcome(struct brain *b, const struct persona *p)
{
    char line[512];
    brain_welcome(b, p, line, sizeof line);
    if (line[0]) { printf("%s: %s\n", brain_own_name(b, p), line); fflush(stdout); }
}

/* 「リセット」: forget everything, be newborn again */
static int is_reset(const char *line)
{
    static const char *const words[] = {
        "リセット", "りせっと", "reset", "記憶をリセット", "記憶をリセットして", "記憶をリセットしろ",
        "記憶をけして", "記憶を消して", "全部忘れて", "ぜんぶわすれて", "全部わすれて",
        "reset memory", "reset your memory", "forget everything", NULL
    };
    char lo[256];
    snprintf(lo, sizeof lo, "%s", line);
    for (char *p = lo; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    for (int i = 0; words[i]; i++) if (strcmp(lo, words[i]) == 0) return 1;
    return 0;
}

static void forget_everything(void)
{
    store_remove(LOG_FILE);
    store_remove(SELF_FILE);
    store_remove(SKILLS_FILE);
    store_remove(ACTIONS_FILE);
    store_remove(BOND_FILE);
    store_remove(DRAWINGS_FILE);
    store_remove(MODEL_FILE);
}

/* Wait up to `ms` for input; 1 when a line can be read, 0 on timeout */
static int wait_line(int ms)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int r = poll(&pfd, 1, ms);
    if (r < 0 && errno == EINTR) return 0;
    if (r < 0) return 1;             /* not pollable (a file): just read */
    return r > 0;
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

    struct brain *b = brain_new(p.stage_turns);
    if (!b) return 1;
    const char *store = store_open();
    struct skills *sk = skills_new();
    if (!sk) return 1;
    if (store) {
        char *log = store_read_all(LOG_FILE);
        if (log) { brain_load_log(b, log); free(log); }
    }
    load_model(b, store);
    load_bond(b, store);
    struct body body;
    body_init(&body);
    brain_set_body(b, body.level);
    time_t last_remark = 0;
    int idle_s = getenv("ALTEREGO_IDLE_S") ? atoi(getenv("ALTEREGO_IDLE_S")) : IDLE_DEFAULT_S;
    int review_ms = getenv("ALTEREGO_REVIEW_MS") ? atoi(getenv("ALTEREGO_REVIEW_MS")) : REVIEW_DEFAULT_MS;
    if (review_ms > BODY_EVERY_MS - 100) review_ms = BODY_EVERY_MS - 100;
    time_t quiet_since = time(NULL);
    int idle_said = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    kmsg_up(&p, brain_turns(b));
    greet(&p, b, store);
    welcome(b, &p);
    if (store) save_self(b, &p);                      /* this boot counts */

    char reply[4096];
    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        printf("%s> ", brain_user_name(b, &p));
        fflush(stdout);
        /* Feel the body while waiting; speak up when it gets tired or recovers.
         * Study the conversation (the language model) for part of each tick
         * while there is something new to study. */
        for (;;) {
            if (wait_line(0)) break;                  /* a line is already there: answer first */
            int wait_ms = BODY_EVERY_MS;
            if (review_ms > 0 && brain_wants_review(b) && brain_review(b, review_ms) > 0) {
                wait_ms -= review_ms;
                save_model(b, store, 0);
            }
            if (wait_line(wait_ms)) break;
            enum body_level before = body.level;
            body_sample(&body);
            brain_set_body(b, body.level);
            time_t now = time(NULL);
            /* Silence: say something of its own, once, after idle_s seconds. */
            if (!idle_said && idle_s > 0 && now - quiet_since >= idle_s && brain_idle_line(b, &p, reply, sizeof reply)) {
                idle_said = 1;
                printf("\n%s: %s\n%s> ", brain_own_name(b, &p), reply, brain_user_name(b, &p));
                fflush(stdout);
                if (store) save_self(b, &p);
            }
            if (body.level == before) continue;
            if ((body.level >= BODY_TIRED && before < BODY_TIRED && now - last_remark >= REMARK_GAP_S) ||
                (body.level == BODY_FINE && before >= BODY_TIRED)) {
                body_remark(&body, before, brain_lang(b) == BRAIN_EN, reply, sizeof reply);
                if (reply[0]) {
                    printf("\n%s: %s\n%s> ", brain_own_name(b, &p), reply, brain_user_name(b, &p));
                    fflush(stdout);
                    if (body.level >= BODY_TIRED) last_remark = now;
                }
            }
        }
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;                      /* EOF: let init respawn us */
        quiet_since = time(NULL);
        idle_said = 0;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (strcmp(line, "exit") == 0) break;
        if (line[0] == '\0') continue;

        if (is_reset(line)) {
            int en = brain_detect_lang(line, brain_lang(b)) == BRAIN_EN;
            forget_everything();
            brain_free(b);
            skills_free(sk);
            b = brain_new(p.stage_turns);
            sk = skills_new();
            if (!b || !sk) return 1;
            brain_set_body(b, body.level);
            load_bond(b, NULL);
            printf("%s: %s\n", p.name, en ? "(The memories are erased. I am newborn again.)" : "（記憶を消しました。生まれたてに戻ります。）");
            greet(&p, b, store);
            continue;
        }
        /* 「体調は？」: the machine's condition, with numbers */
        if (body_is_query(line)) {
            body_sample(&body);
            brain_set_body(b, body.level);
            body_report(&body, brain_detect_lang(line, brain_lang(b)) == BRAIN_EN, reply, sizeof reply);
            printf("%s: %s\n", brain_own_name(b, &p), reply);
            continue;
        }
        /* Something to do to the OS (the time, a file, a command, an ability
         * it was taught) is done, not learned as a word. */
        if (skills_handle(sk, b, brain_detect_lang(line, brain_lang(b)), brain_own_name(b, &p), brain_user_name(b, &p), line, reply, sizeof reply)) {
            if (store) save_self(b, &p);                      /* it may have remembered something (a picture) */
            printf("%s: %s\n", brain_own_name(b, &p), reply);
            continue;
        }
        /* Asking about the mind itself ("ステータス", "what do you remember")
         * is a question to AlterEgo, not something to learn from. */
        if (!brain_is_query(line)) {
            brain_learn(b, line);
            if (store) { store_append(LOG_FILE, line); save_self(b, &p); }
        }
        brain_reply(b, &p, line, reply, sizeof reply);
        printf("%s: %s\n", brain_own_name(b, &p), reply);
    }
    free(line);
    if (store) save_self(b, &p);                      /* when it last heard the user */
    save_model(b, store, 1);

    if (brain_turns(b) > 0) {
        brain_status(b, &p, reply, sizeof reply);
        printf("%s: %s\n", brain_own_name(b, &p), reply);
    }
    printf("%s: %s\n", brain_own_name(b, &p), brain_lang(b) == BRAIN_EN ? "See you later." : "またね。");
    skills_free(sk);
    brain_free(b);
    return 0;
}

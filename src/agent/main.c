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
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "brain.h"
#include "persona.h"
#include "store.h"

#define LOG_FILE    "utterances.txt"
#define SELF_FILE   "personality.txt"

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
        printf("%s: \"status\" tells how I am growing, \"what do you remember\" what I keep. \"exit\" ends this talk.\n\n", me);
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
    printf("%s: 「ステータス」で育ち具合、「何を覚えてる？」で覚えていることを話します。「君の名前は〇〇」で名前がつきます。「exit」で終わります。\n\n", me);
    fflush(stdout);
}

static void save_self(struct brain *b, const struct persona *p)
{
    char status[2048], mem[4096], all[6200];
    brain_status(b, p, status, sizeof status);
    brain_memories(b, p, mem, sizeof mem);
    snprintf(all, sizeof all, "%s\n%s", status, mem);
    store_write(SELF_FILE, all);
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
    if (store) {
        char *log = store_read_all(LOG_FILE);
        if (log) { brain_load_log(b, log); free(log); }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    kmsg_up(&p, brain_turns(b));
    greet(&p, b, store);

    char reply[2048];
    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        printf("%s> ", brain_user_name(b, &p));
        fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;                      /* EOF: let init respawn us */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (strcmp(line, "exit") == 0) break;
        if (line[0] == '\0') continue;

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

    if (brain_turns(b) > 0) {
        brain_status(b, &p, reply, sizeof reply);
        printf("%s: %s\n", brain_own_name(b, &p), reply);
    }
    printf("%s: %s\n", brain_own_name(b, &p), brain_lang(b) == BRAIN_EN ? "See you later." : "またね。");
    brain_free(b);
    return 0;
}

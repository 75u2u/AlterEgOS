/*
 * AlterEgo agent.
 *
 * PID 1 starts it on the console. It reads the persona, rebuilds its brain
 * from the utterance log in the store (the EFI partition it booted from),
 * greets, and talks. Every line the user types is learned first, then
 * answered by AlterEgo's own engine (brain.c): early on in its own polite
 * voice, more and more in the user's as the log grows. It answers in the
 * language it is spoken to, Japanese or English. No external model, no
 * network.
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

#define LOG_FILE   "utterances.txt"
#define SELF_FILE  "self_model.txt"

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
    if (brain_lang(b) == BRAIN_EN) {        /* the last thing わたし said was English: carry on in it */
        printf("\n%s: Good morning, %s. I am %s.\n", p->name, p->address_user_en, p->name);
        if (p->mirror_user)
            printf("%s: I remember %d things you said. Projection is at %d%%.\n",
                   p->name, brain_turns(b), brain_mirror_percent(b));
        if (!store)
            printf("%s: There is nowhere to write, so what I learn today is lost at the next boot.\n", p->name);
        printf("%s: Ask \"what do you know about me\" to hear what I have learned. Type \"exit\" and init starts me again.\n\n", p->name);
        fflush(stdout);
        return;
    }
    printf("\n%s: おはようございます、%s。%sです。\n", p->name, p->address_user, p->name_ja);
    if (p->mirror_user) {
        if (brain_turns(b) > 0)
            printf("%s: これまでに %d 回分の%sの言葉を覚えています。投影は %d%% です。\n",
                   p->name, brain_turns(b), p->address_user, brain_mirror_percent(b));
        else
            printf("%s: 話すたびに%sの性格と口癖を覚えて、%sに近づいていきます。\n",
                   p->name, p->address_user, p->address_user);
    }
    if (!store)
        printf("%s: 書き込める場所が無いので、今日覚えたことは次に起きたら忘れてしまいます。\n", p->name);
    printf("%s: 「わたし像」と聞くと覚えたことを話します。英語で話しかければ英語で返します。「exit」で終わると、initが私をもう一度起動します。\n\n", p->name);
    fflush(stdout);
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

    struct brain *b = brain_new(p.mirror_after);
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
        printf("%s> ", brain_lang(b) == BRAIN_EN ? p.address_user_en : p.address_user);
        fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;                      /* EOF: let init respawn us */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (strcmp(line, "exit") == 0) break;
        if (line[0] == '\0') continue;

        /* Asking about the self-model is a command to AlterEgo, not a
         * sample of how わたし talks: answer it without learning it. */
        if (p.mirror_user && !brain_is_query(line)) {
            brain_learn(b, line);
            if (store) {
                store_append(LOG_FILE, line);
                brain_self_model(b, &p, reply, sizeof reply);
                store_write(SELF_FILE, reply);
            }
        }
        brain_reply(b, &p, line, reply, sizeof reply);
        printf("%s: %s\n", p.name, reply);
    }
    free(line);

    if (p.mirror_user && brain_turns(b) > 0) {
        brain_self_model(b, &p, reply, sizeof reply);
        printf("%s: %s\n", p.name, reply);
    }
    printf("%s: %s\n", p.name, brain_lang(b) == BRAIN_EN ? "See you later." : "また後で。");
    brain_free(b);
    return 0;
}

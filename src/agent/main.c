/*
 * AlterEgo agent, day-1 stub.
 *
 * On day 1 the agent only proves the plumbing: PID 1 started it on the
 * console, the persona file is readable, and a line typed by the user comes
 * back. Days 2-4 replace the echo loop with the network, the model, memory
 * and the syscall/exec tools; this file keeps the same entry point so init
 * never has to change.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "persona.h"

/* One line to the kernel log so a serial console, dmesg or the CI boot test
 * can see the agent came up even when its own tty is the VGA screen.
 * Silently does nothing where /dev/kmsg is unavailable (host runs). */
static void kmsg_up(const struct persona *p)
{
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    const char *tty = ttyname(STDIN_FILENO);
    dprintf(fd, "<6>alterego: up, persona %s, talking on %s\n", p->name, tty ? tty : "?");
    close(fd);
}

static void greet(const struct persona *p)
{
    /* The greeting is the persona's job, not init's: it is the first thing
     * that should feel like AlterEgo rather than like a boot log. */
    printf("\n%s: おはようございます、%s。%sです。\n", p->name, p->address_user, p->name_ja);
    printf("%s: まだ会話はできません。今は入力をそのまま返すだけです。\n", p->name);
    printf("%s: 「exit」で終わると、initが私をもう一度起動します。\n\n", p->name);
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

    setvbuf(stdout, NULL, _IOLBF, 0);
    kmsg_up(&p);
    greet(&p);

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
        printf("%s: 「%s」ですね。\n", p.name, line);
    }
    free(line);
    printf("%s: また後で。\n", p.name);
    return 0;
}

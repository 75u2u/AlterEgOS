/*
 * AlterEgOS init (PID 1)
 *
 * A small supervisor. It does not talk, think, or call the LLM; that is the
 * agent's job. PID 1 only:
 *   1. mounts the pseudo filesystems the agent needs,
 *   2. brings up the loopback interface and sets the hostname,
 *   3. spawns the agent on the console (or, when the console is a screen,
 *      the desktop that runs the agent and a shell in windows), an escape
 *      shell on tty2, and a DHCP client on eth0 when there is one,
 *   4. reaps orphans and respawns the agent (and the shell) when they die,
 *   5. shuts the machine down cleanly on SIGTERM / SIGUSR1 / SIGINT.
 *
 * Keeping PID 1 this small is deliberate: if PID 1 dies the kernel panics,
 * so anything that can fail (network, TLS, SQLite, the model) lives in the
 * agent process instead.
 *
 * Build: see Makefile. Static, no dependencies beyond libc.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "init.h"

#define HOSTNAME        "alteregos"
#define AGENT_PATH      "/usr/bin/alterego"
#define SCREEN_PATH     "/usr/bin/alterego-screen"
#define SCREEN_TTY      "/dev/tty1"
#define AGENT_CONSOLE   "/dev/console"
#define SHELL_PATH      "/bin/sh"
#define SHELL_TTY       "/dev/tty2"
#define DHCP_PATH       "/sbin/udhcpc"
#define DHCP_IF         "eth0"
#define RESPAWN_DELAY_S 1
#define RESPAWN_BURST   5   /* this many deaths within RESPAWN_WINDOW_S ... */
#define RESPAWN_WINDOW_S 30 /* ... and we back off instead of looping hot */
#define RESPAWN_BACKOFF_S 10

static volatile sig_atomic_t g_shutdown = 0;   /* which signal asked us to stop */

/* ------------------------------------------------------------------------ */
/* logging: PID 1 writes to the console with a fixed prefix so the agent's    */
/* own output is easy to tell apart, and the same line to /dev/kmsg so it     */
/* reaches every kernel console (a serial line watching a VGA boot, dmesg).   */

static void logf_(const char *fmt, ...)
{
    char msg[480];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof msg) n = (int)sizeof msg - 1;

    char line[512];
    int len = snprintf(line, sizeof line, "[init] %.*s\n", n, msg);
    int fd = open(AGENT_CONSOLE, O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) fd = STDERR_FILENO;
    (void)!write(fd, line, (size_t)len);
    if (fd != STDERR_FILENO) close(fd);

    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        len = snprintf(line, sizeof line, "<6>alteregos-init: %.*s\n", n, msg);
        (void)!write(kfd, line, (size_t)len);
        close(kfd);
    }
}

/* Where the agent should talk. The kernel's primary console (the last
 * `console=` on the command line, the last entry in
 * /sys/class/tty/console/active) decides: a VT (tty0) means a screen and a
 * keyboard, so the agent gets its own virtual terminal, tty1; anything else
 * (a serial line under QEMU -nographic, a real UART) means /dev/console. */
static const char *pick_agent_tty(void)
{
    char buf[128];
    int fd = open("/sys/class/tty/console/active", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return AGENT_CONSOLE;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return AGENT_CONSOLE;
    buf[n] = '\0';

    char *last = NULL;
    for (char *tok = strtok(buf, " \n"); tok; tok = strtok(NULL, " \n")) last = tok;
    if (last && strcmp(last, "tty0") == 0 && access("/dev/tty1", F_OK) == 0) {
        return "/dev/tty1";
    }
    return AGENT_CONSOLE;
}

/* ------------------------------------------------------------------------ */
/* 1. filesystems                                                             */

struct mnt {
    const char *src, *dst, *type;
    unsigned long flags;
    const char *data;
};

static const struct mnt k_mounts[] = {
    { "proc",     "/proc",    "proc",     MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL },
    { "sysfs",    "/sys",     "sysfs",    MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL },
    { "devtmpfs", "/dev",     "devtmpfs", MS_NOSUID,                         "mode=0755" },
    { "devpts",   "/dev/pts", "devpts",   MS_NOSUID | MS_NOEXEC,             "gid=5,mode=0620,ptmxmode=0666" },
    { "tmpfs",    "/run",     "tmpfs",    MS_NOSUID | MS_NODEV,              "mode=0755" },
    { "tmpfs",    "/tmp",     "tmpfs",    MS_NOSUID | MS_NODEV,              "mode=1777" },
};

static void mount_all(void)
{
    for (size_t i = 0; i < sizeof k_mounts / sizeof k_mounts[0]; i++) {
        const struct mnt *m = &k_mounts[i];
        (void)mkdir(m->dst, 0755);
        if (mount(m->src, m->dst, m->type, m->flags, m->data) < 0 && errno != EBUSY) {
            logf_("mount %s on %s failed: %s", m->type, m->dst, strerror(errno));
        }
    }
    (void)mkdir("/dev/shm", 01777);
    (void)mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
}

/* ------------------------------------------------------------------------ */
/* 2. identity and loopback                                                   */

static void if_up(const char *name)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        logf_("socket for %s failed: %s", name, strerror(errno));
        return;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
            logf_("%s up failed: %s", name, strerror(errno));
        }
    }
    close(fd);
}

static int have_if(const char *name)
{
    char path[80];
    snprintf(path, sizeof path, "/sys/class/net/%s", name);
    return access(path, F_OK) == 0;
}

static void set_identity(void)
{
    if (sethostname(HOSTNAME, strlen(HOSTNAME)) < 0) {
        logf_("sethostname failed: %s", strerror(errno));
    }
    if_up("lo");
    if (have_if(DHCP_IF)) if_up(DHCP_IF);
}

/* ------------------------------------------------------------------------ */
/* 3. children                                                                */

enum child_kind { CHILD_AGENT = 0, CHILD_SHELL = 1, CHILD_DHCP = 2, CHILD_SCREEN = 3, CHILD_COUNT };

#define CHILD_ARGV_MAX 8

struct child {
    const char *name;
    const char *path;
    const char *tty;                /* NULL: stdio on /dev/null (a daemon) */
    const char *argv[CHILD_ARGV_MAX];
    pid_t pid;
    time_t deaths[RESPAWN_BURST];   /* ring of recent exit times */
    int death_idx;
    time_t next_spawn;              /* earliest time to respawn */
};

static struct child g_children[CHILD_COUNT] = {
    [CHILD_AGENT] = { "agent", AGENT_PATH, AGENT_CONSOLE, { AGENT_PATH, NULL }, 0, {0}, 0, 0 },
    [CHILD_SHELL] = { "shell", SHELL_PATH, SHELL_TTY,     { SHELL_PATH, NULL }, 0, {0}, 0, 0 },
    /* busybox udhcpc in the foreground (it keeps renewing the lease); its
     * default script configures the interface and writes /etc/resolv.conf */
    [CHILD_DHCP]  = { "dhcp",  DHCP_PATH,  NULL, { DHCP_PATH, "-i", DHCP_IF, "-f", NULL }, 0, {0}, 0, 0 },
    /* the desktop on the framebuffer; it starts the agent and a shell itself */
    [CHILD_SCREEN] = { "screen", SCREEN_PATH, SCREEN_TTY, { SCREEN_PATH, NULL }, 0, {0}, 0, 0 },
};

/* Open `tty`, make it the controlling terminal and stdio of the calling
 * (child) process. Returns -1 if the device does not exist, so a missing
 * tty2 (serial-only QEMU) just means no escape shell. */
static int attach_tty(const char *tty)
{
    if (!tty) {
        int fd = open("/dev/null", O_RDWR);
        if (fd < 0) return -1;
        (void)setsid();
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
        return 0;
    }
    int fd = open(tty, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
    (void)setsid();
    (void)ioctl(fd, TIOCSCTTY, 1);
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

static void spawn(struct child *c)
{
    if (access(c->path, X_OK) != 0) {
        /* Nothing to run: say so once and stop trying. */
        if (c->next_spawn != (time_t)-1) {
            logf_("%s: %s is missing, not starting it", c->name, c->path);
        }
        c->next_spawn = (time_t)-1;
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        logf_("fork for %s failed: %s", c->name, strerror(errno));
        c->next_spawn = time(NULL) + RESPAWN_DELAY_S;
        return;
    }
    if (pid == 0) {
        /* child */
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, NULL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        if (attach_tty(c->tty) < 0) {
            _exit(127);  /* no such tty: treated below as "do not respawn" */
        }
        char *const envp[] = {
            "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
            "HOME=/root",
            "TERM=linux",
            "ALTEREGO_PERSONA=/etc/alterego/persona.toml",
            NULL,
        };
        execve(c->path, (char *const *)c->argv, envp);
        _exit(126);
    }
    c->pid = pid;
}

/* Called when a tracked child exits. Decides when (or whether) to respawn. */
static void on_child_exit(struct child *c, int status)
{
    c->pid = 0;
    time_t now = time(NULL);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        logf_("%s: %s not available, not respawning", c->name, c->tty ? c->tty : "/dev/null");
        c->next_spawn = (time_t)-1;
        if (c == &g_children[CHILD_SCREEN] && g_children[CHILD_AGENT].next_spawn == (time_t)-1) {
            /* no framebuffer after all: the agent talks on the console instead */
            logf_("no screen, starting the agent on %s", g_children[CHILD_AGENT].tty);
            g_children[CHILD_AGENT].next_spawn = now;
        }
        return;
    }
    if (WIFSIGNALED(status)) {
        logf_("%s died from signal %d", c->name, WTERMSIG(status));
    } else {
        logf_("%s exited with status %d", c->name, WEXITSTATUS(status));
    }

    /* Hot-loop guard: RESPAWN_BURST deaths inside RESPAWN_WINDOW_S → back off. */
    c->deaths[c->death_idx] = now;
    c->death_idx = (c->death_idx + 1) % RESPAWN_BURST;
    time_t oldest = c->deaths[c->death_idx];
    if (oldest != 0 && now - oldest < RESPAWN_WINDOW_S) {
        logf_("%s keeps dying, waiting %ds before the next try", c->name, RESPAWN_BACKOFF_S);
        c->next_spawn = now + RESPAWN_BACKOFF_S;
    } else {
        c->next_spawn = now + RESPAWN_DELAY_S;
    }
}

static void reap(void)
{
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        for (int i = 0; i < CHILD_COUNT; i++) {
            if (g_children[i].pid == pid) {
                on_child_exit(&g_children[i], status);
                break;
            }
        }
        /* untracked orphans are simply reaped */
    }
}

static void respawn_due(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < CHILD_COUNT; i++) {
        struct child *c = &g_children[i];
        if (c->pid == 0 && c->next_spawn != (time_t)-1 && now >= c->next_spawn) {
            spawn(c);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* 4. signals and shutdown                                                    */

static void on_signal(int sig)
{
    g_shutdown = sig;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);   /* poweroff */
    sigaction(SIGUSR1, &sa, NULL);   /* halt */
    sigaction(SIGINT,  &sa, NULL);   /* ctrl-alt-del → reboot */
    sa.sa_handler = SIG_DFL;         /* SIGCHLD just wakes the loop */
    sigaction(SIGCHLD, &sa, NULL);
    reboot(RB_DISABLE_CAD);          /* deliver ctrl-alt-del as SIGINT */
}

static void shutdown_machine(int sig)
{
    logf_("shutting down (signal %d)", sig);
    kill(-1, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD) break;
        usleep(100 * 1000);
    }
    kill(-1, SIGKILL);
    sync();
    (void)umount2("/", MNT_DETACH);
    int cmd = (sig == SIGINT) ? RB_AUTOBOOT
            : (sig == SIGUSR1) ? RB_HALT_SYSTEM
            : RB_POWER_OFF;
    reboot(cmd);
    /* if reboot() returns we are not PID 1 (host test); just exit */
    _exit(0);
}

/* ------------------------------------------------------------------------ */

int alteregos_init_main(void)
{
    install_signals();
    mount_all();
    set_identity();
    g_children[CHILD_AGENT].tty = pick_agent_tty();
    logf_("AlterEgOS " ALTEREGOS_VERSION " booted, starting the agent on %s",
          g_children[CHILD_AGENT].tty);

    if (!have_if(DHCP_IF)) {
        logf_("no %s, not starting the DHCP client", DHCP_IF);
        g_children[CHILD_DHCP].next_spawn = (time_t)-1;
    }
    /* A screen (console on a VT) gets the desktop, which runs the agent in a
     * window; the agent itself is started here only if the desktop finds no
     * framebuffer (it exits 127). A serial console gets the agent directly. */
    if (strcmp(g_children[CHILD_AGENT].tty, SCREEN_TTY) == 0 && access(SCREEN_PATH, X_OK) == 0) {
        logf_("console is a screen, starting the desktop on %s", SCREEN_TTY);
        g_children[CHILD_AGENT].next_spawn = (time_t)-1;
    } else {
        g_children[CHILD_SCREEN].next_spawn = (time_t)-1;
    }
    for (int i = 0; i < CHILD_COUNT; i++) {
        if (g_children[i].next_spawn != (time_t)-1) spawn(&g_children[i]);
    }

    /* Main loop: sleep until a signal (SIGCHLD or shutdown), reap, respawn.
     * sigsuspend-free version: a short sleep keeps the loop simple and the
     * respawn delay bounded; PID 1 is otherwise idle. */
    for (;;) {
        if (g_shutdown) shutdown_machine(g_shutdown);
        reap();
        respawn_due();
        sleep(1);
    }
}

#ifndef ALTEREGOS_INIT_NO_MAIN
int main(void)
{
    if (getpid() != 1) {
        fprintf(stderr, "alteregos-init: must run as PID 1 (use init=/sbin/init)\n");
        return 1;
    }
    return alteregos_init_main();
}
#endif

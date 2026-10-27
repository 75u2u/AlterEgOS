#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "pty.h"

int pty_spawn(char *const argv[], char *const envp[], int cols, int rows, pid_t *pid)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m < 0) return -1;
    if (grantpt(m) < 0 || unlockpt(m) < 0) { close(m); return -1; }
    char name[64];
    if (ptsname_r(m, name, sizeof name) != 0) { close(m); return -1; }

    struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols, .ws_xpixel = 0, .ws_ypixel = 0 };
    pid_t p = fork();
    if (p < 0) { close(m); return -1; }
    if (p == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        setsid();
        int s = open(name, O_RDWR);
        if (s < 0) _exit(126);
        ioctl(s, TIOCSCTTY, 0);
        ioctl(s, TIOCSWINSZ, &ws);
        struct termios tio;
        if (tcgetattr(s, &tio) == 0) {
            tio.c_iflag |= IUTF8;        /* erase whole UTF-8 characters, not bytes */
            tcsetattr(s, TCSANOW, &tio);
        }
        dup2(s, STDIN_FILENO);
        dup2(s, STDOUT_FILENO);
        dup2(s, STDERR_FILENO);
        if (s > STDERR_FILENO) close(s);
        execve(argv[0], argv, envp);
        _exit(126);
    }
    *pid = p;
    return m;
}

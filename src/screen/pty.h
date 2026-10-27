#ifndef ALTEREGO_PTY_H
#define ALTEREGO_PTY_H

#include <sys/types.h>

/* Run a program on a new pseudo-terminal of cols x rows, UTF-8 aware.
 * Returns the master fd (-1 on error) and the child's pid. */
int pty_spawn(char *const argv[], char *const envp[], int cols, int rows, pid_t *pid);

#endif

#ifndef ALTEREGO_PROCNET_H
#define ALTEREGO_PROCNET_H

/* Who is talking to the outside: the sockets in /proc/net/{tcp,tcp6,udp,udp6}
 * that have a remote end (not listening, not loopback), and which process
 * holds each one (/proc/<pid>/fd -> socket:[inode]). Shared by the agent
 * (body.c: 「外と話している」) and the desktop (procs.c: the lines to 「外」).
 * `root` is "/proc", or a made-up directory in the tests. */

#define PROCNET_IP 48

struct procnet_conn {
    unsigned long inode;
    char proto;             /* 't' tcp, 'u' udp */
    int v6;
    char ip[PROCNET_IP];    /* the remote address, IPv4-mapped IPv6 shown as IPv4 */
    int port;               /* the remote port */
    int established;        /* tcp ESTABLISHED (udp: always 1) */
};

/* The outbound connections, at most `max`; returns how many. */
int procnet_outbound(const char *root, struct procnet_conn *c, int max);
/* Which of `c[0..n)` process `pid` holds: their indexes into `idx`, at most `max`; returns how many.
 * Cheap when n is 0 (nothing to look for). */
int procnet_of_pid(const char *root, int pid, const struct procnet_conn *c, int n, int *idx, int max);

#endif

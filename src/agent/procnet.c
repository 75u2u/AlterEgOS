/* Outbound sockets and who holds them (see procnet.h). */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "procnet.h"

/* A /proc/net address: hex, each 32-bit word in host byte order (little
 * endian on every machine this OS runs on). "0100007F:0050" = 127.0.0.1:80. */
static int parse_addr(const char *s, int v6, char *ip, size_t cap, int *port, int *loop, int *zero)
{
    const char *colon = strchr(s, ':');
    if (!colon) return 0;
    size_t hexlen = (size_t)(colon - s);
    if (hexlen != (v6 ? 32u : 8u)) return 0;
    *port = (int)strtol(colon + 1, NULL, 16);
    unsigned b[16];
    for (size_t i = 0; i < hexlen / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0;
        b[i] = v;
    }
    /* each 4-byte word is stored reversed */
    unsigned a[16];
    for (size_t w = 0; w < hexlen / 8; w++)
        for (int k = 0; k < 4; k++) a[w * 4 + k] = b[w * 4 + 3 - k];
    *zero = 1;
    for (size_t i = 0; i < hexlen / 2; i++) if (a[i]) *zero = 0;
    if (!v6) {
        *loop = a[0] == 127;
        snprintf(ip, cap, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
        return 1;
    }
    int mapped = 1;
    for (int i = 0; i < 10; i++) if (a[i]) mapped = 0;
    if (mapped && a[10] == 255 && a[11] == 255) {               /* ::ffff:1.2.3.4 */
        *loop = a[12] == 127;
        snprintf(ip, cap, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
        return 1;
    }
    int l = 1;
    for (int i = 0; i < 15; i++) if (a[i]) l = 0;
    *loop = l && a[15] == 1;                                    /* ::1 */
    size_t o = 0;
    for (int i = 0; i < 8 && o < cap; i++) o += (size_t)snprintf(ip + o, cap - o, "%s%x", i ? ":" : "", (a[2 * i] << 8) | a[2 * i + 1]);
    return 1;
}

static int read_table(const char *root, const char *file, char proto, int v6, struct procnet_conn *c, int n, int max)
{
    char path[600], line[512];
    snprintf(path, sizeof path, "%s/net/%s", root, file);
    FILE *f = fopen(path, "r");
    if (!f) return n;
    if (!fgets(line, sizeof line, f)) { fclose(f); return n; }          /* the header */
    while (n < max && fgets(line, sizeof line, f)) {
        char local[64], rem[64];
        unsigned st = 0;
        unsigned long inode = 0;
        if (sscanf(line, " %*d: %63s %63s %x %*s %*s %*s %*d %*d %lu", local, rem, &st, &inode) < 4) continue;
        if (st == 0x0A || !inode) continue;                             /* listening, or gone */
        int port = 0, loop = 0, zero = 0;
        if (!parse_addr(rem, v6, c[n].ip, sizeof c[n].ip, &port, &loop, &zero)) continue;
        if (zero || loop) continue;                                     /* no remote end, or ourselves */
        c[n].inode = inode;
        c[n].proto = proto;
        c[n].v6 = v6;
        c[n].port = port;
        c[n].established = proto == 'u' || st == 0x01;
        n++;
    }
    fclose(f);
    return n;
}

int procnet_outbound(const char *root, struct procnet_conn *c, int max)
{
    int n = 0;
    n = read_table(root, "tcp", 't', 0, c, n, max);
    n = read_table(root, "tcp6", 't', 1, c, n, max);
    n = read_table(root, "udp", 'u', 0, c, n, max);
    n = read_table(root, "udp6", 'u', 1, c, n, max);
    return n;
}

int procnet_of_pid(const char *root, int pid, const struct procnet_conn *c, int n, int *idx, int max)
{
    if (n <= 0 || max <= 0) return 0;
    char path[600];
    snprintf(path, sizeof path, "%s/%d/fd", root, pid);
    DIR *d = opendir(path);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while (found < max && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char link[700], target[64];
        snprintf(link, sizeof link, "%s/%.32s", path, e->d_name);
        ssize_t k = readlink(link, target, sizeof target - 1);
        if (k <= 0) continue;
        target[k] = '\0';
        unsigned long inode;
        if (sscanf(target, "socket:[%lu]", &inode) != 1) continue;
        for (int i = 0; i < n && found < max; i++) if (c[i].inode == inode) { idx[found++] = i; break; }
    }
    closedir(d);
    return found;
}

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "store.h"

#define MNT "/run/alterego/esp"

static char g_dir[256];

static int is_esp(const char *mnt)
{
    char p[300];
    snprintf(p, sizeof p, "%s/EFI", mnt);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int try_mount(const char *dev)
{
    if (mount(dev, MNT, "vfat", MS_NOATIME, "utf8=1") == 0 ||
        mount(dev, MNT, "vfat", MS_NOATIME | MS_RDONLY, "utf8=1") == 0) {
        if (is_esp(MNT)) return 0;
        umount(MNT);
    }
    return -1;
}

const char *store_open(void)
{
    if (g_dir[0]) return g_dir;

    const char *env = getenv("ALTEREGO_STATE");
    if (env && *env) {
        (void)mkdir(env, 0700);
        snprintf(g_dir, sizeof g_dir, "%s", env);
        return g_dir;
    }

    FILE *f = fopen("/proc/partitions", "r");
    if (!f) return NULL;
    (void)mkdir("/run/alterego", 0755);
    (void)mkdir(MNT, 0755);
    char line[128];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        unsigned major, minor;
        unsigned long blocks;
        char name[64];
        if (sscanf(line, " %u %u %lu %63s", &major, &minor, &blocks, name) != 4) continue;
        if (name[strlen(name) - 1] < '0' || name[strlen(name) - 1] > '9') continue;  /* partitions only */
        char dev[80];
        snprintf(dev, sizeof dev, "/dev/%s", name);
        if (try_mount(dev) == 0) found = 1;
    }
    fclose(f);
    if (!found) return NULL;

    snprintf(g_dir, sizeof g_dir, "%s/alterego", MNT);
    (void)mkdir(g_dir, 0700);
    return g_dir;
}

int store_read(const char *name, char *buf, size_t cap)
{
    if (!g_dir[0]) return -1;
    char p[320];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

int store_write(const char *name, const char *data)
{
    if (!g_dir[0]) return -1;
    char p[320], tmp[330];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(data);
    ssize_t n = write(fd, data, len);
    int ok = (n == (ssize_t)len) && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, p) < 0) { unlink(tmp); return -1; }
    sync();   /* a USB stick may be pulled right after */
    return 0;
}

int store_append(const char *name, const char *line)
{
    if (!g_dir[0]) return -1;
    char p[320];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(line);
    int ok = write(fd, line, len) == (ssize_t)len && write(fd, "\n", 1) == 1 && fsync(fd) == 0;
    close(fd);
    return ok ? 0 : -1;
}

char *store_read_all(const char *name)
{
    if (!g_dir[0]) return NULL;
    char p[320];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            char *q = realloc(buf, cap);
            if (!q) { free(buf); fclose(f); return NULL; }
            buf = q;
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    fclose(f);
    if (!buf) return strdup("");
    buf[len] = '\0';
    return buf;
}

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "store.h"

#define MNT "/run/alterego/esp"

static char g_dir[256];

/* A disk that scripts/run-qemu.sh made for AlterEgo's memory: empty but for
 * this mark at its start. It is formatted (FAT) the first time, and only a
 * disk carrying the mark is ever formatted, so a real PC's disks are safe. */
#define STATE_MAGIC "ALTEREGO-STATE\n"

static void klog(const char *msg)
{
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    dprintf(fd, "<6>alterego: %s\n", msg);
    close(fd);
}

static int has_dir(const char *mnt, const char *name)
{
    char p[300];
    snprintf(p, sizeof p, "%s/%s", mnt, name);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The ESP it booted from (EFI/), or a memory disk formatted earlier (alterego/) */
static int is_ours(const char *mnt)
{
    return has_dir(mnt, "EFI") || has_dir(mnt, "alterego");
}

static int try_mount(const char *dev)
{
    if (mount(dev, MNT, "vfat", MS_NOATIME, "utf8=1") == 0 ||
        mount(dev, MNT, "vfat", MS_NOATIME | MS_RDONLY, "utf8=1") == 0) {
        if (is_ours(MNT)) return 0;
        umount(MNT);
    }
    return -1;
}

static int has_magic(const char *dev)
{
    char buf[sizeof STATE_MAGIC];
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof STATE_MAGIC - 1);
    close(fd);
    return n == (ssize_t)(sizeof STATE_MAGIC - 1) && memcmp(buf, STATE_MAGIC, sizeof STATE_MAGIC - 1) == 0;
}

/* mkfs.vfat (busybox) on the marked disk, then mount it */
static int format_and_mount(const char *dev)
{
    static const char *const mkfs[][3] = {
        { "/sbin/mkfs.vfat", NULL, NULL },
        { "/usr/sbin/mkfs.vfat", NULL, NULL },
        { "/bin/busybox", "mkfs.vfat", NULL },
    };
    for (size_t i = 0; i < sizeof mkfs / sizeof mkfs[0]; i++) {
        if (access(mkfs[i][0], X_OK) != 0) continue;
        pid_t pid = fork();
        if (pid == 0) {
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
            if (mkfs[i][1]) execl(mkfs[i][0], mkfs[i][0], mkfs[i][1], "-n", "ALTEREGO", dev, (char *)NULL);
            else execl(mkfs[i][0], mkfs[i][0], "-n", "ALTEREGO", dev, (char *)NULL);
            _exit(127);
        }
        if (pid < 0) return -1;
        int st = 0;
        waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
        if (mount(dev, MNT, "vfat", MS_NOATIME, "utf8=1") != 0) return -1;
        char msg[160];
        snprintf(msg, sizeof msg, "memory disk %s formatted, memories will survive reboots", dev);
        klog(msg);
        return 0;
    }
    return -1;
}

/* First the partitions (the ESP of the boot USB/disk), then whole disks
 * (a memory disk QEMU attaches, formatted or still only marked). */
static int scan(int whole_disks)
{
    FILE *f = fopen("/proc/partitions", "r");
    if (!f) return 0;
    char line[128];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        unsigned major, minor;
        unsigned long blocks;
        char name[64];
        if (sscanf(line, " %u %u %lu %63s", &major, &minor, &blocks, name) != 4) continue;
        int is_part = name[strlen(name) - 1] >= '0' && name[strlen(name) - 1] <= '9';
        if (is_part == whole_disks) continue;
        char dev[80];
        snprintf(dev, sizeof dev, "/dev/%s", name);
        if (try_mount(dev) == 0) found = 1;
        else if (whole_disks && has_magic(dev) && format_and_mount(dev) == 0) found = 1;
    }
    fclose(f);
    return found;
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

    (void)mkdir("/run/alterego", 0755);
    (void)mkdir(MNT, 0755);
    /* the disks may still be probing right after boot: a few tries */
    int found = 0;
    for (int try = 0; try < 6 && !found; try++) {
        if (try) usleep(500000);
        found = scan(0) || scan(1);
        if (geteuid() != 0) break;
    }
    if (!found) return NULL;

    snprintf(g_dir, sizeof g_dir, "%s/alterego", MNT);
    (void)mkdir(g_dir, 0700);
    return g_dir;
}

int store_remove(const char *name)
{
    if (!g_dir[0]) return -1;
    char p[320];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    int r = unlink(p);
    sync();
    return r == 0 || errno == ENOENT ? 0 : -1;
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

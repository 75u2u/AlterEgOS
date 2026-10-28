#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "body.h"

static int has(const char *s, const char *n) { return strstr(s, n) != NULL; }

void body_init(struct body *b)
{
    memset(b, 0, sizeof *b);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    b->ncpu = n > 0 ? (int)n : 1;
    b->disk = -1;
    body_sample(b);
}

static void sample_cpu(struct body *b)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    unsigned long long v[10] = {0};
    char line[256];
    if (fgets(line, sizeof line, f) && strncmp(line, "cpu ", 4) == 0) {
        sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        unsigned long long idle = v[3] + v[4], total = 0;
        for (int i = 0; i < 8; i++) total += v[i];
        unsigned long long busy = total - idle;
        if (b->prev_total && total > b->prev_total) {
            b->delta_total = total - b->prev_total;
            b->cpu = (int)((busy - b->prev_busy) * 100 / b->delta_total);
            b->sampled = 1;
        }
        b->prev_total = total;
        b->prev_busy = busy;
    }
    fclose(f);
}

static void sample_mem(struct body *b)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    unsigned long total = 0, avail = 0, freek = 0;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %lu", &total) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu", &avail) == 1) continue;
        if (sscanf(line, "MemFree: %lu", &freek) == 1) continue;
    }
    fclose(f);
    if (!avail) avail = freek;
    if (total) b->mem = (int)(avail * 100 / total);
}

static void sample_load(struct body *b)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;
    double l1 = 0;
    int running = 0, total = 0;
    if (fscanf(f, "%lf %*f %*f %d/%d", &l1, &running, &total) >= 1) b->load = l1;
    if (total) b->procs = total;
    fclose(f);
    struct statvfs sv;
    if (statvfs("/", &sv) == 0 && sv.f_blocks) b->disk = (int)(sv.f_bavail * 100 / sv.f_blocks);
}

/* Every process' CPU ticks; the one that grew most since the last sample is
 * the heaviest. The table remembers the previous ticks per pid. */
static void sample_procs(struct body *b)
{
    DIR *d = opendir("/proc");
    if (!d) return;
    struct { int pid; unsigned long long ticks; } now[512];
    int n = 0, count = 0;
    unsigned long long best = 0;
    int best_i = -1;
    char best_name[64] = "";
    struct dirent *e;
    while ((e = readdir(d)) && n < 512) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        count++;
        char path[64];
        snprintf(path, sizeof path, "/proc/%.20s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }
        fclose(f);
        /* pid (comm) state ppid ... utime(14) stime(15); comm may hold spaces */
        char *rp = strrchr(line, ')');
        char *lp = strchr(line, '(');
        if (!rp || !lp || rp < lp) continue;
        char name[64];
        size_t nl = (size_t)(rp - lp - 1);
        if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, lp + 1, nl);
        name[nl] = '\0';
        unsigned long long ut = 0, st = 0;
        if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        now[n].pid = atoi(e->d_name);
        now[n].ticks = ut + st;
        unsigned long long prev = 0;
        for (int i = 0; i < b->nseen; i++) if (b->seen[i].pid == now[n].pid) { prev = b->seen[i].ticks; break; }
        unsigned long long grew = now[n].ticks > prev ? now[n].ticks - prev : 0;
        if (b->nseen && grew > best) { best = grew; best_i = n; snprintf(best_name, sizeof best_name, "%s", name); }
        n++;
    }
    closedir(d);
    if (count) b->procs = count;
    memcpy(b->seen, now, sizeof now[0] * (size_t)n);
    b->nseen = n;
    if (best_i >= 0 && b->delta_total) {
        snprintf(b->top, sizeof b->top, "%s", best_name);
        b->top_pid = now[best_i].pid;
        /* share of one CPU: ticks grown over the interval of one CPU */
        b->top_cpu = (int)(best * 100 * (unsigned long long)b->ncpu / b->delta_total);
        if (b->top_cpu > 100) b->top_cpu = 100;
    } else {
        b->top[0] = '\0';
        b->top_cpu = 0;
    }
}

enum body_level body_sample(struct body *b)
{
    sample_cpu(b);
    sample_mem(b);
    sample_load(b);
    sample_procs(b);
    int cpu = b->sampled ? b->cpu : 0;
    enum body_level lv = BODY_FINE;
    if (cpu > 60 || b->mem < 30 || b->load > b->ncpu) lv = BODY_HEAVY;
    if (cpu > 85 || b->mem < 15 || b->load > 2.0 * b->ncpu) lv = BODY_TIRED;
    if (b->mem < 5 || (b->disk >= 0 && b->disk < 5)) lv = BODY_SICK;
    b->level = lv;
    return lv;
}

int body_is_query(const char *line)
{
    char lo[256];
    snprintf(lo, sizeof lo, "%s", line);
    for (char *p = lo; *p; p++) *p = (char)tolower((unsigned char)*p);
    int asks = has(line, "？") || has(line, "?") || has(line, "どう") || has(line, "教えて") || has(line, "おしえて") || has(line, "見せて") || has(line, "みせて");
    if (has(line, "体調") || has(line, "たいちょう")) return 1;
    if ((has(line, "調子") || has(line, "ちょうし")) && !has(line, "好調")) return 1;
    if (asks && (has(line, "負荷") || has(lo, "cpu") || has(line, "メモリ") || has(line, "めもり") || has(line, "重い") || has(line, "おもい"))) return 1;
    if (has(lo, "how is your body") || has(lo, "how's your body") || has(lo, "how do you feel") || has(lo, "system load") ||
        has(lo, "load average") || has(lo, "memory usage") || has(lo, "memory load") || has(lo, "cpu usage") || has(lo, "cpu load")) return 1;
    return 0;
}

const char *body_word(const struct body *b, int en)
{
    static const char *const ja[] = { "元気", "ちょっと重い", "疲れている", "苦しい" };
    static const char *const eng[] = { "fine", "a bit heavy", "tired", "struggling" };
    return (en ? eng : ja)[b->level];
}

void body_report(const struct body *b, int en, char *out, size_t cap)
{
    char top[128] = "";
    if (b->top[0] && b->top_cpu >= 5)
        snprintf(top, sizeof top, en ? " The heaviest is %s (%d%% of a CPU)." : "いちばん重いのは %s（CPU の %d%%）。", b->top, b->top_cpu);
    char disk[64] = "";
    if (b->disk >= 0) snprintf(disk, sizeof disk, en ? ", disk %d%% free" : "、ディスク空き %d%%", b->disk);
    if (en)
        snprintf(out, cap, "I'm %s. CPU %d%% busy, memory %d%% free%s, load %.2f, %d processes.%s",
                 body_word(b, 1), b->sampled ? b->cpu : 0, b->mem, disk, b->load, b->procs, top);
    else
        snprintf(out, cap, "いまは%s。CPU 使用率 %d%%、メモリ空き %d%%%s、負荷 %.2f、プロセス %d 個。%s",
                 body_word(b, 0), b->sampled ? b->cpu : 0, b->mem, disk, b->load, b->procs, top);
}

void body_remark(const struct body *b, enum body_level before, int en, char *out, size_t cap)
{
    out[0] = '\0';
    if (b->level == before) return;
    char top[128] = "";
    if (b->top[0] && b->top_cpu >= 5)
        snprintf(top, sizeof top, en ? " %s is working hard (%d%% of a CPU)." : "%s が頑張っている（CPU の %d%%）。", b->top, b->top_cpu);
    if (b->level >= BODY_TIRED && before < BODY_TIRED) {
        if (b->level == BODY_SICK)
            snprintf(out, cap, en ? "...ugh. I'm struggling: memory %d%% free, disk %d%% free.%s" : "……うっ。苦しい。メモリ空き %d%%、ディスク空き %d%%。%s",
                     b->mem, b->disk < 0 ? 100 : b->disk, top);
        else
            snprintf(out, cap, en ? "...phew, I'm getting tired. CPU %d%%, memory %d%% free.%s" : "……ふう、疲れてきた。CPU %d%%、メモリ空き %d%%。%s",
                     b->sampled ? b->cpu : 0, b->mem, top);
    } else if (b->level == BODY_FINE && before >= BODY_TIRED) {
        snprintf(out, cap, en ? "...ah, that's better. I'm fine again." : "……ふう、楽になった。もう元気。");
    }
}

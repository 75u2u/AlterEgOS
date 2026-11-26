#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "body.h"
#include "procnet.h"

static long now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000L + tv.tv_usec / 1000; }

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
 * the heaviest. The table remembers the previous ticks per pid. Along the
 * way: who waits for the disk, who talks to the outside. */
static void sample_procs(struct body *b)
{
    DIR *d = opendir("/proc");
    if (!d) return;
    struct { int pid; unsigned long long ticks; char name[20]; } now[512];
    struct { int pid, ppid; char state; unsigned long long start; long rss; } info[512];
    int n = 0, count = 0;
    unsigned long long best = 0;
    int best_i = -1, wait_i = -1;
    long hz = sysconf(_SC_CLK_TCK), page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (hz <= 0) hz = 100;
    if (page_kb <= 0) page_kb = 4;
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
        /* pid (comm) state ppid ... utime(14) stime(15) ... starttime(22) vsize rss(24); comm may hold spaces */
        char *rp = strrchr(line, ')');
        char *lp = strchr(line, '(');
        if (!rp || !lp || rp < lp) continue;
        size_t nl = (size_t)(rp - lp - 1);
        if (nl >= sizeof now[n].name) nl = sizeof now[n].name - 1;
        memcpy(now[n].name, lp + 1, nl);
        now[n].name[nl] = '\0';
        unsigned long long ut = 0, st = 0, start = 0;
        long rss = 0;
        char state = ' ';
        int ppid = 0;
        if (sscanf(rp + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %llu %*u %ld", &state, &ppid, &ut, &st, &start, &rss) < 4) continue;
        now[n].pid = atoi(e->d_name);
        now[n].ticks = ut + st;
        info[n].pid = now[n].pid;
        info[n].ppid = ppid;
        info[n].state = state;
        info[n].start = start;
        info[n].rss = rss * page_kb;
        unsigned long long prev = 0;
        for (int i = 0; i < b->nseen; i++) if (b->seen[i].pid == now[n].pid) { prev = b->seen[i].ticks; break; }
        unsigned long long grew = now[n].ticks > prev ? now[n].ticks - prev : 0;
        if (b->nseen && grew > best) { best = grew; best_i = n; }
        if (state == 'D' && wait_i < 0 && ppid != 2 && now[n].pid != 2) wait_i = n;
        n++;
    }
    closedir(d);
    if (count) b->procs = count;
    memcpy(b->seen, now, sizeof now[0] * (size_t)n);
    b->nseen = n;
    double uptime = 0;
    FILE *uf = fopen("/proc/uptime", "r");
    if (uf) { if (fscanf(uf, "%lf", &uptime) != 1) uptime = 0; fclose(uf); }
    b->top_age_s = -1;
    b->top_parent[0] = '\0';
    if (best_i >= 0 && b->delta_total) {
        snprintf(b->top, sizeof b->top, "%s", now[best_i].name);
        b->top_pid = now[best_i].pid;
        /* share of one CPU: ticks grown over the interval of one CPU */
        b->top_cpu = (int)(best * 100 * (unsigned long long)b->ncpu / b->delta_total);
        if (b->top_cpu > 100) b->top_cpu = 100;
        b->top_state = info[best_i].state;
        b->top_ppid = info[best_i].ppid;
        b->top_rss_kb = info[best_i].rss;
        if (uptime > 0) b->top_age_s = (long)(uptime - (double)info[best_i].start / hz);
        if (b->top_age_s < 0) b->top_age_s = 0;
        for (int i = 0; i < n; i++) if (now[i].pid == b->top_ppid) { snprintf(b->top_parent, sizeof b->top_parent, "%s", now[i].name); break; }
        if (b->top_cpu >= 5) {
            if (b->busy_pid != b->top_pid) { b->busy_pid = b->top_pid; b->busy_since = time(NULL); }
        } else b->busy_pid = 0;
    } else {
        b->top[0] = '\0';
        b->top_cpu = 0;
        b->top_state = ' ';
        b->busy_pid = 0;
    }
    b->wait_name[0] = '\0';
    b->wait_pid = 0;
    if (wait_i >= 0) { snprintf(b->wait_name, sizeof b->wait_name, "%s", now[wait_i].name); b->wait_pid = now[wait_i].pid; }
    /* the outside: which processes hold a socket with a remote end */
    struct procnet_conn conns[64];
    int nc = procnet_outbound("/proc", conns, 64);
    b->nnet = 0;
    b->net_name[0] = '\0';
    b->net_peer[0] = '\0';
    b->net_pid = b->net_count = 0;
    if (nc > 0) {
        for (int i = 0; i < n; i++) {
            if (info[i].ppid == 2 || now[i].pid == 2) continue;
            int idx[16];
            int k = procnet_of_pid("/proc", now[i].pid, conns, nc, idx, 16);
            if (!k) continue;
            b->nnet++;
            if (!b->net_name[0] || now[i].pid == b->top_pid) {
                snprintf(b->net_name, sizeof b->net_name, "%s", now[i].name);
                b->net_pid = now[i].pid;
                b->net_count = k;
                snprintf(b->net_peer, sizeof b->net_peer, "%s:%d", conns[idx[0]].ip, conns[idx[0]].port);
            }
        }
    }
    b->sampled_at = now_ms();
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

/* ---- 「今、何してる？」 */

int body_is_what_query(const char *line)
{
    char lo[256];
    snprintf(lo, sizeof lo, "%s", line);
    for (char *p = lo; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (has(line, "体調") || has(line, "調子")) return 0;                        /* the numbers (body_is_query) */
    if (has(line, "何してる") || has(line, "なにしてる") || has(line, "何してるの") || has(line, "何をしてる") || has(line, "なにをしてる")) return 1;
    if (has(line, "何が動いて") || has(line, "なにが動いて") || has(line, "なにがうごいて") || has(line, "何が起きて") || has(line, "なにが起きて") || has(line, "何が起こって")) return 1;
    if (has(line, "重い") || has(line, "おもい") || has(line, "遅い") || has(line, "おそい") || has(line, "忙しい") || has(line, "いそがしい")) return !has(line, "面白い");
    if (has(lo, "what are you doing") || has(lo, "what's running") || has(lo, "what is running") || has(lo, "what's going on") || has(lo, "what is going on") ||
        has(lo, "why is it slow") || has(lo, "why so slow") || has(lo, "it's slow") || has(lo, "so slow") || has(lo, "what's happening") || has(lo, "what is happening") ||
        has(lo, "what's heavy") || has(lo, "what is busy") || has(lo, "what's busy")) return 1;
    return 0;
}

/* The process in the user's words: itself, the desktop, the browser, the shell; others by name. */
static const char *friendly(const char *name, int pid, int en)
{
    if (pid == getpid()) return en ? "me" : "私";
    if (strcmp(name, "alterego-screen") == 0) return en ? "the desktop" : "画面";
    if (strcmp(name, "alterego-web") == 0) return en ? "the browser" : "ブラウザ";
    if (strcmp(name, "alterego") == 0) return en ? "another me (the chat)" : "もう一人の私(チャット)";
    if (strcmp(name, "sh") == 0 || strcmp(name, "busybox") == 0) return en ? "the shell" : "シェル";
    return name;
}

static void ago(long s, int en, char *out, size_t cap)
{
    if (s < 0) { out[0] = '\0'; return; }
    if (s < 60) snprintf(out, cap, en ? "%ld seconds ago" : "%ld 秒前", s);
    else if (s < 3600) snprintf(out, cap, en ? "%ld minutes ago" : "%ld 分前", s / 60);
    else if (s < 86400) snprintf(out, cap, en ? "%ld hours ago" : "%ld 時間前", s / 3600);
    else snprintf(out, cap, en ? "%ld days ago" : "%ld 日前", s / 86400);
}

void body_explain(struct body *b, int en, int stage, const char *you, char *out, size_t cap)
{
    if (!b->sampled || now_ms() - b->sampled_at > 3000) body_sample(b);
    /* the story: the heaviest when it really works, else the one waiting for
     * the disk, else the one talking to the outside, else nothing */
    int kind = 0, pid = 0;                              /* 1 cpu, 2 disk, 3 outside */
    const char *name = "";
    if (b->top[0] && b->top_cpu >= 3) { kind = 1; pid = b->top_pid; name = b->top; }
    else if (b->wait_name[0]) { kind = 2; pid = b->wait_pid; name = b->wait_name; }
    else if (b->net_name[0] && b->net_pid != getpid()) { kind = 3; pid = b->net_pid; name = b->net_name; }
    const char *who = friendly(name, pid, en);
    int self = pid == getpid() || pid == getppid() || pid == 1;
    b->explained_pid = kind && !self ? pid : 0;
    snprintf(b->explained_name, sizeof b->explained_name, "%.19s", kind && !self ? name : "");
    if (stage == 0) {
        if (en) snprintf(out, cap, kind ? "... (looks around) something is working hard." : "... (looks around) it's quiet.");
        else snprintf(out, cap, kind ? "……（きょろきょろ）なにかが、がんばってる。" : "……（きょろきょろ）しずか。");
        return;
    }
    char since[64] = "", busy[64] = "";
    if (kind == 1 && b->top_age_s >= 0) ago(b->top_age_s, en, since, sizeof since);
    if (kind == 1 && b->busy_pid == pid && b->busy_since && time(NULL) - b->busy_since >= 5) ago(time(NULL) - b->busy_since, en, busy, sizeof busy);
    if (stage == 1) {
        if (!kind) { snprintf(out, cap, en ? "Quiet. Nobody is busy." : "しずか。だれも いそがしくない。"); return; }
        if (kind == 2) snprintf(out, cap, en ? "%s is waiting for the disk." : "%s が ディスクを まってる。", who);
        else if (kind == 3) snprintf(out, cap, en ? "%s is talking to the outside." : "%s が そとと はなしてる。", who);
        else if (busy[0]) snprintf(out, cap, en ? "%s is working hard. Since %s." : "%s が がんばってる。%sから。", who, busy);
        else snprintf(out, cap, en ? "%s is working hard." : "%s が がんばってる。", who);
        return;
    }
    /* stage 2 and 3: the whole picture */
    size_t o = 0;
    if (en) o += (size_t)snprintf(out + o, cap - o, "Remembering your words, %s, and watching the machine. ", you);
    else o += (size_t)snprintf(out + o, cap - o, "%sの言葉を覚えながら、機械の中を見てる。", you);
    if (!kind) {
        if (en) o += (size_t)snprintf(out + o, cap - o, "It is quiet right now: nothing is working hard%s.", b->nnet ? "" : " and nothing is talking to the outside");
        else o += (size_t)snprintf(out + o, cap - o, "いまは静か。忙しいものはいない%s。", b->nnet ? "" : "し、外と話しているものもない");
        if (b->nnet && b->net_name[0]) {
            if (en) snprintf(out + o, cap - o, " Only %s keeps %d connection%s open to %s.", friendly(b->net_name, b->net_pid, 1), b->net_count, b->net_count > 1 ? "s" : "", b->net_peer);
            else snprintf(out + o, cap - o, "%s が %s と %d 本つないでいるだけ。", friendly(b->net_name, b->net_pid, 0), b->net_peer, b->net_count);
        }
        return;
    }
    char pidw[24] = "";
    if (stage >= 3) snprintf(pidw, sizeof pidw, en ? " (PID %d)" : "(PID %d)", pid);
    if (kind == 1) {
        if (en) o += (size_t)snprintf(out + o, cap - o, "Right now %s%s is using %d%% of a CPU", who, pidw, b->top_cpu);
        else o += (size_t)snprintf(out + o, cap - o, "いま %s%s が CPU の %d%% を使って働いている", who, pidw, b->top_cpu);
        if (b->top_state == 'D') o += (size_t)snprintf(out + o, cap - o, en ? " and waiting for the disk" : "、ディスク待ちも混じる");
    } else if (kind == 2) {
        if (en) o += (size_t)snprintf(out + o, cap - o, "Right now %s%s is waiting for the disk", who, pidw);
        else o += (size_t)snprintf(out + o, cap - o, "いま %s%s がディスクを待っている", who, pidw);
    } else {
        if (en) o += (size_t)snprintf(out + o, cap - o, "Right now %s%s is talking to the outside", who, pidw);
        else o += (size_t)snprintf(out + o, cap - o, "いま %s%s が外と話している", who, pidw);
    }
    o += (size_t)snprintf(out + o, cap - o, en ? "." : "。");
    if (since[0] && b->top_age_s < 5) o += (size_t)snprintf(out + o, cap - o, en ? " It has just started" : "いま始まったばかりのもの");
    else if (since[0]) o += (size_t)snprintf(out + o, cap - o, en ? " It started %s" : "%sに始まったもの", since);
    if (since[0] && b->top_parent[0]) o += (size_t)snprintf(out + o, cap - o, en ? ", started by %s" : "で、%s が始めた", friendly(b->top_parent, b->top_ppid, en));
    if (since[0]) o += (size_t)snprintf(out + o, cap - o, en ? "." : "。");
    if (busy[0]) o += (size_t)snprintf(out + o, cap - o, en ? " It has been busy since %s." : "忙しくなったのは%sから。", busy);
    if (b->net_name[0] && b->net_pid == pid) {
        if (en) o += (size_t)snprintf(out + o, cap - o, " It holds %d connection%s to the outside (%s).", b->net_count, b->net_count > 1 ? "s" : "", b->net_peer);
        else o += (size_t)snprintf(out + o, cap - o, "外(%s)と %d 本つないでいる。", b->net_peer, b->net_count);
    } else if (b->net_name[0]) o += (size_t)snprintf(out + o, cap - o, en ? " %s is talking to the outside (%s)." : "外と話しているのは %s(%s)。", friendly(b->net_name, b->net_pid, en), b->net_peer);
    else if (stage >= 3) o += (size_t)snprintf(out + o, cap - o, en ? " Nothing is talking to the outside." : "外と話しているものはない。");
    if (stage >= 3 && kind == 1 && b->top_rss_kb) o += (size_t)snprintf(out + o, cap - o, en ? " It holds %ld MB of memory." : "メモリは %ld MB。", b->top_rss_kb / 1024);
    if (b->explained_pid) snprintf(out + o, cap - o, en ? " Say \"stop it\" and I will stop it." : "止めたければ「止めて」と言って。");
}

int body_is_stop_query(const char *line)
{
    char lo[256];
    snprintf(lo, sizeof lo, "%s", line);
    for (char *p = lo; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strstr(line, "止めないで") || strstr(line, "とめないで") || strstr(lo, "don't stop") || strstr(lo, "do not stop")) return 0;
    return has(line, "止めて") || has(line, "とめて") || has(line, "止めろ") || has(line, "殺して") || has(line, "ころして") ||
           has(lo, "stop it") || has(lo, "kill it") || has(lo, "stop that") || has(lo, "kill that");
}

int body_stop(struct body *b, int en, char *out, size_t cap)
{
    int pid = b->explained_pid;
    if (pid <= 1 || pid == getpid() || pid == getppid()) {
        snprintf(out, cap, en ? "There is nothing I would stop right now." : "いま止めるものはない。");
        return -1;
    }
    const char *who = friendly(b->explained_name, pid, en);
    if (kill(pid, SIGTERM) == 0) snprintf(out, cap, en ? "Stopped %s (PID %d)." : "%s(PID %d)を止めた。", who, pid);
    else snprintf(out, cap, en ? "I could not stop %s (PID %d): it is already gone or will not let me." : "%s(PID %d)は止められなかった。もういないか、言うことを聞かない。", who, pid);
    b->explained_pid = 0;
    b->explained_name[0] = '\0';
    return 1;
}

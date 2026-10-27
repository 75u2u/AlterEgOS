#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "input.h"

#define BITS(n) (((n) + 7) / 8)
static int bit(const unsigned char *b, int i) { return (b[i / 8] >> (i % 8)) & 1; }

static enum input_kind classify(int fd, struct input_dev *d)
{
    unsigned char ev[BITS(EV_MAX + 1)] = {0}, key[BITS(KEY_MAX + 1)] = {0}, rel[BITS(REL_MAX + 1)] = {0}, abs[BITS(ABS_MAX + 1)] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0) return DEV_NONE;
    if (bit(ev, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
    if (bit(ev, EV_REL)) ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    if (bit(ev, EV_ABS)) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    if (bit(abs, ABS_X) && bit(abs, ABS_Y) && (bit(key, BTN_LEFT) || bit(key, BTN_TOUCH))) {
        struct input_absinfo ax, ay;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0) {
            d->ax_min = ax.minimum; d->ax_max = ax.maximum;
            d->ay_min = ay.minimum; d->ay_max = ay.maximum;
            if (d->ax_max > d->ax_min && d->ay_max > d->ay_min) return DEV_TABLET;
        }
    }
    if (bit(rel, REL_X) && bit(rel, REL_Y) && bit(key, BTN_LEFT)) return DEV_MOUSE;
    if (bit(key, KEY_A) && bit(key, KEY_Z) && bit(key, KEY_ENTER)) return DEV_KEYBOARD;
    return DEV_NONE;
}

void input_init(struct input *in) { memset(in, 0, sizeof *in); }

int input_scan(struct input *in)
{
    DIR *d = opendir("/dev/input");
    if (!d) return in->n;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int known = 0;
        for (int i = 0; i < in->n; i++) if (strcmp(in->dev[i].path, path) == 0) known = 1;
        if (known || in->n >= INPUT_MAX_DEV) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct input_dev *dev = &in->dev[in->n];
        memset(dev, 0, sizeof *dev);
        enum input_kind k = classify(fd, dev);
        if (k == DEV_NONE) { close(fd); continue; }
        ioctl(fd, EVIOCGRAB, (void *)1);
        dev->fd = fd;
        dev->kind = k;
        size_t plen = strlen(path);
        if (plen >= sizeof dev->path) { close(fd); continue; }
        memcpy(dev->path, path, plen + 1);
        in->n++;
    }
    closedir(d);
    return in->n;
}

int input_pollfds(const struct input *in, struct pollfd *pfd, int max)
{
    int n = 0;
    for (int i = 0; i < in->n && n < max; i++) { pfd[n].fd = in->dev[i].fd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
    return n;
}

static void drop(struct input *in, int idx)
{
    close(in->dev[idx].fd);
    memmove(&in->dev[idx], &in->dev[idx + 1], (size_t)(in->n - idx - 1) * sizeof in->dev[0]);
    in->n--;
}

/* US layout: unshifted and shifted character for each key code. */
struct km { int code; const char *s; };
static const struct km keymap[] = {
    {KEY_1,"1!"},{KEY_2,"2@"},{KEY_3,"3#"},{KEY_4,"4$"},{KEY_5,"5%"},{KEY_6,"6^"},{KEY_7,"7&"},{KEY_8,"8*"},{KEY_9,"9("},{KEY_0,"0)"},
    {KEY_MINUS,"-_"},{KEY_EQUAL,"=+"},{KEY_Q,"qQ"},{KEY_W,"wW"},{KEY_E,"eE"},{KEY_R,"rR"},{KEY_T,"tT"},{KEY_Y,"yY"},{KEY_U,"uU"},
    {KEY_I,"iI"},{KEY_O,"oO"},{KEY_P,"pP"},{KEY_LEFTBRACE,"[{"},{KEY_RIGHTBRACE,"]}"},{KEY_A,"aA"},{KEY_S,"sS"},{KEY_D,"dD"},
    {KEY_F,"fF"},{KEY_G,"gG"},{KEY_H,"hH"},{KEY_J,"jJ"},{KEY_K,"kK"},{KEY_L,"lL"},{KEY_SEMICOLON,";:"},{KEY_APOSTROPHE,"'\""},
    {KEY_GRAVE,"`~"},{KEY_BACKSLASH,"\\|"},{KEY_Z,"zZ"},{KEY_X,"xX"},{KEY_C,"cC"},{KEY_V,"vV"},{KEY_B,"bB"},{KEY_N,"nN"},{KEY_M,"mM"},
    {KEY_COMMA,",<"},{KEY_DOT,".>"},{KEY_SLASH,"/?"},{KEY_SPACE,"  "},{KEY_KPASTERISK,"**"},{KEY_KPMINUS,"--"},{KEY_KPPLUS,"++"},
    {KEY_KPSLASH,"//"},{KEY_KPDOT,".."},{KEY_KP0,"00"},{KEY_KP1,"11"},{KEY_KP2,"22"},{KEY_KP3,"33"},{KEY_KP4,"44"},{KEY_KP5,"55"},
    {KEY_KP6,"66"},{KEY_KP7,"77"},{KEY_KP8,"88"},{KEY_KP9,"99"},{KEY_YEN,"\\|"},{KEY_RO,"\\_"},
    {0, NULL}
};

static const char *key_text(struct input *in, int code, char *buf, size_t cap)
{
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: return "\r";
    case KEY_BACKSPACE: return "\x7f";
    case KEY_TAB: return "\t";
    case KEY_ESC: return "\x1b";
    case KEY_UP: return "\x1b[A";
    case KEY_DOWN: return "\x1b[B";
    case KEY_RIGHT: return "\x1b[C";
    case KEY_LEFT: return "\x1b[D";
    case KEY_HOME: return "\x1b[H";
    case KEY_END: return "\x1b[F";
    case KEY_DELETE: return "\x1b[3~";
    case KEY_INSERT: return "\x1b[2~";
    case KEY_PAGEUP: return "\x1b[5~";
    case KEY_PAGEDOWN: return "\x1b[6~";
    default: break;
    }
    for (int i = 0; keymap[i].s; i++) {
        if (keymap[i].code != code) continue;
        int ch = keymap[i].s[in->shift ? 1 : 0];
        int letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (letter && in->caps) ch = in->shift ? keymap[i].s[0] : keymap[i].s[1];
        if (in->ctrl) {
            if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;
            else if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 1;
            else if (ch == '[' || ch == '3') ch = 0x1b;
            else if (ch == '\\' || ch == '4') ch = 0x1c;
            else if (ch == ']' || ch == '5') ch = 0x1d;
            else if (ch == '^' || ch == '6') ch = 0x1e;
            else if (ch == '_' || ch == '7' || ch == '-') ch = 0x1f;
            else if (ch == ' ' || ch == '2' || ch == '@') ch = 0;
            else return NULL;
        }
        if (ch == 0) return NULL;
        buf[0] = (char)ch;
        buf[1] = '\0';
        (void)cap;
        return buf;
    }
    return NULL;
}

int input_read(struct input *in, int idx, struct input_ev *ev, int max)
{
    struct input_dev *d = &in->dev[idx];
    struct input_event raw[32];
    ssize_t n = read(d->fd, raw, sizeof raw);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        drop(in, idx);
        return -1;
    }
    int out = 0;
    for (size_t i = 0; i < (size_t)n / sizeof raw[0] && out < max; i++) {
        const struct input_event *e = &raw[i];
        if (e->type == EV_KEY) {
            int down = e->value != 0;
            switch (e->code) {
            case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: in->shift = down; continue;
            case KEY_LEFTCTRL: case KEY_RIGHTCTRL: in->ctrl = down; continue;
            case KEY_LEFTALT: case KEY_RIGHTALT: in->alt = down; continue;
            case KEY_CAPSLOCK: if (e->value == 1) in->caps = !in->caps; continue;
            default: break;
            }
            if (e->code >= BTN_MOUSE && e->code < BTN_JOYSTICK) {
                int b = e->code == BTN_LEFT ? 0 : e->code == BTN_RIGHT ? 1 : e->code == BTN_MIDDLE ? 2 : -1;
                if (b < 0 || e->value == 2) continue;
                ev[out].type = IN_BUTTON; ev[out].button = b; ev[out].down = down; out++;
                continue;
            }
            if (e->code == BTN_TOUCH) {
                if (e->value == 2) continue;
                ev[out].type = IN_BUTTON; ev[out].button = 0; ev[out].down = down; out++;
                continue;
            }
            if (!down) continue;                      /* releases of ordinary keys */
            memset(&ev[out], 0, sizeof ev[out]);
            ev[out].type = IN_KEY;
            ev[out].key = (int)e->code;
            ev[out].ctrl = in->ctrl; ev[out].alt = in->alt; ev[out].shift = in->shift;
            out++;
            if (out >= max) break;
            char buf[2];
            const char *t = key_text(in, (int)e->code, buf, sizeof buf);
            if (t) {
                memset(&ev[out], 0, sizeof ev[out]);
                ev[out].type = IN_TEXT;
                ev[out].key = (int)e->code;
                ev[out].ctrl = in->ctrl; ev[out].alt = in->alt; ev[out].shift = in->shift;
                snprintf(ev[out].text, sizeof ev[out].text, "%s", t);
                out++;
            }
        } else if (e->type == EV_REL) {
            if (e->code == REL_X) d->dx += e->value;
            else if (e->code == REL_Y) d->dy += e->value;
            else if (e->code == REL_WHEEL) d->wheel += e->value;
        } else if (e->type == EV_ABS) {
            if (e->code == ABS_X) { d->ax = e->value; d->have_abs = 1; }
            else if (e->code == ABS_Y) { d->ay = e->value; d->have_abs = 1; }
        } else if (e->type == EV_SYN) {
            if (d->dx || d->dy) {
                memset(&ev[out], 0, sizeof ev[out]);
                ev[out].type = IN_MOUSE_MOVE; ev[out].dx = d->dx; ev[out].dy = d->dy; out++;
                d->dx = d->dy = 0;
            }
            if (d->have_abs && out < max) {
                memset(&ev[out], 0, sizeof ev[out]);
                ev[out].type = IN_MOUSE_ABS;
                ev[out].ax = d->ax - d->ax_min; ev[out].ay = d->ay - d->ay_min;
                ev[out].ax_max = d->ax_max - d->ax_min; ev[out].ay_max = d->ay_max - d->ay_min;
                out++;
                d->have_abs = 0;
            }
            if (d->wheel && out < max) {
                memset(&ev[out], 0, sizeof ev[out]);
                ev[out].type = IN_WHEEL; ev[out].wheel = d->wheel; out++;
                d->wheel = 0;
            }
        }
    }
    return out;
}

void input_close(struct input *in)
{
    for (int i = 0; i < in->n; i++) { ioctl(in->dev[i].fd, EVIOCGRAB, (void *)0); close(in->dev[i].fd); }
    in->n = 0;
}

#ifndef ALTEREGO_INPUT_H
#define ALTEREGO_INPUT_H

#include <poll.h>
#include <stdint.h>

/* Keyboards and mice straight from the kernel (evdev, /dev/input/event*),
 * grabbed so the virtual console does not also act on the keys. Keys are
 * translated with a US layout into what a terminal would send. */

#define INPUT_MAX_DEV 16

enum input_kind { DEV_NONE = 0, DEV_KEYBOARD, DEV_MOUSE, DEV_TABLET };

struct input_dev {
    int fd;
    char path[64];
    enum input_kind kind;
    int dx, dy, wheel;                  /* pending until EV_SYN */
    int ax, ay, have_abs;
    int ax_min, ax_max, ay_min, ay_max;
};

enum input_type { IN_NONE = 0, IN_KEY, IN_TEXT, IN_MOUSE_MOVE, IN_MOUSE_ABS, IN_BUTTON, IN_WHEEL };

struct input_ev {
    enum input_type type;
    int key;                            /* IN_KEY: evdev keycode (down or repeat) */
    int ctrl, alt, shift;
    char text[8];                       /* IN_TEXT: what the key sends (ASCII or an escape sequence) */
    int dx, dy;                         /* IN_MOUSE_MOVE */
    int ax, ay, ax_max, ay_max;         /* IN_MOUSE_ABS: position in device units */
    int button, down;                   /* IN_BUTTON: 0 left, 1 right, 2 middle */
    int wheel;                          /* IN_WHEEL: +up / -down */
};

struct input {
    struct input_dev dev[INPUT_MAX_DEV];
    int n;
    int shift, ctrl, alt, caps;
};

void input_init(struct input *in);
int  input_scan(struct input *in);                     /* open new devices; returns how many are open */
int  input_pollfds(const struct input *in, struct pollfd *pfd, int max);
/* Read one device after poll said it is readable; fills up to `max` events. -1: device gone. */
int  input_read(struct input *in, int idx, struct input_ev *ev, int max);
void input_close(struct input *in);

#endif

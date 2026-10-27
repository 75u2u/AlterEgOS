#ifndef ALTEREGO_FB_H
#define ALTEREGO_FB_H

#include <stddef.h>
#include <stdint.h>

#include "canvas.h"

/* The Linux framebuffer (/dev/fb0): whatever the kernel set up for the
 * screen, EFI/VESA on a PC, the DRM driver's fbdev emulation under QEMU
 * and on the Pi. The canvas is converted to its pixel format on blit. */
struct fb {
    int fd;
    uint8_t *mem;
    size_t len;
    int w, h, bpp, stride;          /* stride in bytes */
    int ro, rl, go, gl, bo, bl;     /* channel offsets / lengths */
};

int  fb_open(struct fb *f, const char *dev);      /* -1 and errno on failure */
void fb_close(struct fb *f);
void fb_blit(struct fb *f, const struct canvas *c);

#endif

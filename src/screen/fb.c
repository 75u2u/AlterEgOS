#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fb.h"

int fb_open(struct fb *f, const char *dev)
{
    memset(f, 0, sizeof *f);
    f->fd = open(dev, O_RDWR | O_CLOEXEC);
    if (f->fd < 0) return -1;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(f->fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(f->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        close(f->fd);
        return -1;
    }
    if (var.bits_per_pixel != 16 && var.bits_per_pixel != 24 && var.bits_per_pixel != 32) {
        close(f->fd);
        errno = ENOTSUP;
        return -1;
    }
    f->w = (int)var.xres;
    f->h = (int)var.yres;
    f->bpp = (int)var.bits_per_pixel;
    f->stride = (int)fix.line_length;
    f->ro = (int)var.red.offset;   f->rl = (int)var.red.length;
    f->go = (int)var.green.offset; f->gl = (int)var.green.length;
    f->bo = (int)var.blue.offset;  f->bl = (int)var.blue.length;
    f->len = (size_t)f->stride * (size_t)(var.yres + var.yoffset);
    f->mem = mmap(NULL, f->len, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->mem == MAP_FAILED) {
        close(f->fd);
        return -1;
    }
    f->mem += (size_t)var.yoffset * (size_t)f->stride;
    return 0;
}

void fb_close(struct fb *f)
{
    if (f->mem) munmap(f->mem, f->len);
    if (f->fd >= 0) close(f->fd);
    f->mem = NULL;
    f->fd = -1;
}

static inline uint32_t pack(const struct fb *f, uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r >> (8 - f->rl)) << f->ro) | ((g >> (8 - f->gl)) << f->go) | ((b >> (8 - f->bl)) << f->bo);
}

void fb_blit(struct fb *f, const struct canvas *c)
{
    int w = c->w < f->w ? c->w : f->w;
    int h = c->h < f->h ? c->h : f->h;
    int fast32 = f->bpp == 32 && f->ro == 16 && f->go == 8 && f->bo == 0 && f->rl == 8 && f->gl == 8 && f->bl == 8;
    for (int y = 0; y < h; y++) {
        const uint32_t *src = c->px + (size_t)y * c->w;
        uint8_t *dst = f->mem + (size_t)y * f->stride;
        if (fast32) {
            memcpy(dst, src, (size_t)w * 4);
        } else if (f->bpp == 32) {
            uint32_t *d = (uint32_t *)dst;
            for (int x = 0; x < w; x++) d[x] = pack(f, src[x]);
        } else if (f->bpp == 16) {
            uint16_t *d = (uint16_t *)dst;
            for (int x = 0; x < w; x++) d[x] = (uint16_t)pack(f, src[x]);
        } else {
            for (int x = 0; x < w; x++) {
                uint32_t v = pack(f, src[x]);
                dst[x * 3] = (uint8_t)v; dst[x * 3 + 1] = (uint8_t)(v >> 8); dst[x * 3 + 2] = (uint8_t)(v >> 16);
            }
        }
    }
}

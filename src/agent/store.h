#ifndef ALTEREGO_STORE_H
#define ALTEREGO_STORE_H

#include <stddef.h>

/* Where AlterEgo keeps what must survive a reboot before the btrfs root
 * exists: a directory `alterego/` on the EFI System Partition of the disk or
 * USB stick it booted from (found by trying every block partition for a FAT
 * filesystem with an EFI/ directory), or $ALTEREGO_STATE on a host. Holds
 * the API key and the learned self-model. NULL when nothing is writable
 * (QEMU -kernel/-initrd boots): then everything lives for one session. */

const char *store_open(void);                        /* dir path or NULL */
int store_read(const char *name, char *buf, size_t cap);   /* bytes or -1 */
int store_write(const char *name, const char *data);       /* 0 or -1 */

#endif

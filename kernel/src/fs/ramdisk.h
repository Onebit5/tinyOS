#ifndef FS_RAMDISK_H
#define FS_RAMDISK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* a read-only filesystem that is just a tar file limine handed us at
 * boot. ustar is about as simple as a container format gets: a 512 byte
 * header of ascii fields, the file's bytes rounded up to 512, repeat,
 * and two blocks of zeroes to finish.
 *
 * there is no writing, no directories to speak of, and no allocation --
 * ramdisk_open hands back a pointer straight into the archive. it is a
 * filesystem in the sense that a filing cabinet is furniture */

struct ramdisk_file {
    const char *name;
    const void *data;
    uint64_t    size;

    /* the unix mode tar recorded. we only ever look at one bit of it --
     * whether the world may read -- but that one bit is enough to give
     * a uid something it can and cannot do */
    uint32_t    mode;
};

/* may a process running as `uid` read this file? uid 0 may read
 * anything; everybody else needs the other-read bit */
bool ramdisk_may_read(const struct ramdisk_file *f, int uid);

/* take the archive from limine. must run before the bootloader memory
 * is reclaimed, since the module list lives in it */
void ramdisk_init(void);

/* point at an archive directly. the guts, so tests can hand it bytes */
void ramdisk_mount(const void *base, uint64_t size);

bool     ramdisk_present(void);
uint64_t ramdisk_bytes(void);

/* walk the archive. index from 0 until it returns false */
bool ramdisk_stat(size_t index, struct ramdisk_file *out);

/* find one by name. returns false if it isnt there */
bool ramdisk_open(const char *name, struct ramdisk_file *out);

size_t ramdisk_count(void);

#endif

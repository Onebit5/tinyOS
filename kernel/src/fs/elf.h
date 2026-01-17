#ifndef FS_ELF_H
#define FS_ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* just enough elf64 to load a static executable. no dynamic linking, no
 * relocation, no sections -- program headers are all a loader needs */

struct elf_load_result {
    uint64_t entry;         /* where to start executing */
    uint64_t brk;           /* first address past everything I mapped */
    bool     ok;
    const char *error;      /* why not, when ok is false */
};

/* check the header without loading anything. split out so it can be
 * tested against handcrafted files, and so the loader can refuse
 * politely rather than mapping nonsense */
bool elf_is_loadable(const void *image, uint64_t size, const char **why);

/* map every PT_LOAD segment into the given address space with user
 * permissions, copying from the image. frames come from the pmm.
 *
 * the space does not have to be the live one: the copy goes through the
 * direct map, which is mapped identically everywhere */
struct elf_load_result elf_load(const void *image, uint64_t size,
                                uint64_t pml4);

#endif

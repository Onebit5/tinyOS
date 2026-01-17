#ifndef LIB_KSYMS_H
#define LIB_KSYMS_H

#include <stdint.h>
#include <stddef.h>

/* the kernel's own symbol table, baked in at build time by
 * tools/gensyms.py so a panic can say where it happened in words.
 *
 * names are offsets into one big blob rather than pointers, because
 * pointers would need relocating and the whole table has to survive
 * being generated from one link and used in the next */

struct ksym {
    uint64_t addr;
    uint32_t name_off;
};

/* generated. absent from host builds, which supply their own */
extern const struct ksym ksym_table[];
extern const char ksym_names[];
extern const unsigned long ksym_count;

/* which function contains addr, and how far in. returns NULL if the
 * address is outside every function I know about */
const char *ksym_lookup(uint64_t addr, uint64_t *offset);

/* the same search against a table you supply -- this is the part with
 * the logic in it, and the part the tests exercise */
const char *ksym_lookup_in(const struct ksym *table, size_t count,
                           const char *names, uint64_t addr,
                           uint64_t *offset);

#endif

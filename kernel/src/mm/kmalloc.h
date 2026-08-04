#ifndef MM_KMALLOC_H
#define MM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

/* the kernel heap. first-fit free list that grabs whole pages from the
 * pmm when it runs low. 16-byte aligned payloads, headers with magic
 * so kfree can tell honest pointers from lies */

void *kmalloc(size_t size);
void  kfree(void *ptr);

/* stats. used includes block headers, its the honest number */
uint64_t kheap_total_bytes(void);
uint64_t kheap_used_bytes(void);

#endif

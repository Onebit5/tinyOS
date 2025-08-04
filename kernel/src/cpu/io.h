#ifndef CPU_IO_H
#define CPU_IO_H

#include <stdint.h>

/* port io wrappers. the "Nd" constraint lets gcc encode ports < 256 as an
 * immediate instead of going through dx, not that it matters much */

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %b0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %w1, %b0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

#endif

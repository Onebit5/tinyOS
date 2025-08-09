#ifndef CPU_MSR_H
#define CPU_MSR_H

#include <stdint.h>

/* model specific registers and cpuid. only a couple of each so far */

#define MSR_EFER      0xc0000080
#define EFER_NXE      (1ull << 11)   /* honour the no-execute bit */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile ("wrmsr"
                  :
                  : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    asm volatile ("cpuid"
                  : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                  : "a"(leaf), "c"(0));
}

#endif

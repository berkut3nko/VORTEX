/* The start-up CPU check (CpuCheck.c): plain C, outside the module folders, compiled without the
 * engine's instruction-set flags. Every program that links vortex_cpu_check refuses to start on a CPU
 * without the instructions the build assumes (exit code 4 and a message) instead of crashing with an
 * illegal instruction somewhere later. */
#ifndef VORTEX_CPU_CHECK_H
#define VORTEX_CPU_CHECK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The CPUID words the check reads. Leaves above the reported maximum read as 0. */
typedef struct VortexCpuBits {
    unsigned maxLeaf;            /* CPUID.0:EAX */
    unsigned maxExtendedLeaf;    /* CPUID.80000000h:EAX */
    unsigned leaf1Ecx;           /* CPUID.1:ECX */
    unsigned leaf7Ebx;           /* CPUID.(7,0):EBX */
    unsigned extended1Ecx;       /* CPUID.80000001h:ECX */
    unsigned long long xcr0;     /* XGETBV(0); read only when CPUID.1:ECX.OSXSAVE is set, else 0 */
} VortexCpuBits;

/* The features the build needs, as bits of the masks below. */
enum {
    VORTEX_CPU_SSE41 = 1u << 0,
    VORTEX_CPU_SSE42 = 1u << 1,
    VORTEX_CPU_POPCNT = 1u << 2,
    VORTEX_CPU_AVX = 1u << 3,
    VORTEX_CPU_AVX2 = 1u << 4,
    VORTEX_CPU_FMA = 1u << 5,
    VORTEX_CPU_BMI1 = 1u << 6,
    VORTEX_CPU_LZCNT = 1u << 7,
    VORTEX_CPU_F16C = 1u << 8
};

/* The features `bits` lacks (a mask of VORTEX_CPU_*): AVX, AVX2, FMA and F16C also count as missing
 * when the operating system does not save the AVX state (OSXSAVE clear, or XCR0 without SSE and AVX). */
unsigned vortex_cpu_missing(const VortexCpuBits* bits);
/* The names in `mask` ("AVX2, FMA"), written into out (always terminated); returns the length. */
size_t vortex_cpu_names(unsigned mask, char* out, size_t size);
/* The mask of names in a comma-separated list ("avx2,lzcnt", any case): the test hook
 * VORTEX_CPU_CHECK_PRETEND_MISSING. */
unsigned vortex_cpu_parse_names(const char* list);

#ifdef __cplusplus
}
#endif

#endif

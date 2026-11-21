/* The start-up CPU check (CpuCheck.h). It runs before every C++ static initializer of the program
 * (a priority-101 constructor; on the MSVC ABI a .CRT$XCT entry, ahead of the compiler's .CRT$XCU),
 * so no code built with -mavx2 / -mfma / -mbmi / -mlzcnt / -mf16c / -mpopcnt runs first.
 * VORTEX_CPU_CHECK_PRETEND_MISSING=avx2[,fma...] makes it act as if those were missing (a test hook). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#   define _GNU_SOURCE   /* program_invocation_short_name */
#endif

#include "CpuCheck.h"

#include <ctype.h>
#include <errno.h>   /* program_invocation_short_name (glibc, _GNU_SOURCE) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) || defined(__FMA__) || defined(__BMI__) || defined(__LZCNT__) || defined(__F16C__)
#   error "CpuCheck.c must be compiled without the engine's instruction-set flags (it runs on any x86-64 CPU)"
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#   define VORTEX_CPU_X86 1
#   if defined(__GNUC__) || defined(__clang__)
#       include <cpuid.h>
#   else
#       include <intrin.h>
#   endif
#else
#   define VORTEX_CPU_X86 0
#endif

static const struct {
    unsigned bit;
    const char* name;
} kNames[] = {
    {VORTEX_CPU_SSE41, "SSE4.1"}, {VORTEX_CPU_SSE42, "SSE4.2"}, {VORTEX_CPU_POPCNT, "POPCNT"}, {VORTEX_CPU_AVX, "AVX"},
    {VORTEX_CPU_AVX2, "AVX2"},    {VORTEX_CPU_FMA, "FMA"},      {VORTEX_CPU_BMI1, "BMI1"},     {VORTEX_CPU_LZCNT, "LZCNT"},
    {VORTEX_CPU_F16C, "F16C"},
};

unsigned vortex_cpu_missing(const VortexCpuBits* b) {
    unsigned missing = 0;
    const unsigned ecx = b->maxLeaf >= 1 ? b->leaf1Ecx : 0;
    if (!(ecx & (1u << 19))) missing |= VORTEX_CPU_SSE41;
    if (!(ecx & (1u << 20))) missing |= VORTEX_CPU_SSE42;
    if (!(ecx & (1u << 23))) missing |= VORTEX_CPU_POPCNT;
    if (!(ecx & (1u << 28))) missing |= VORTEX_CPU_AVX;
    if (!(ecx & (1u << 12))) missing |= VORTEX_CPU_FMA;
    if (!(ecx & (1u << 29))) missing |= VORTEX_CPU_F16C;
    const unsigned ebx7 = b->maxLeaf >= 7 ? b->leaf7Ebx : 0;
    if (!(ebx7 & (1u << 5))) missing |= VORTEX_CPU_AVX2;
    if (!(ebx7 & (1u << 3))) missing |= VORTEX_CPU_BMI1;
    const unsigned ext = b->maxExtendedLeaf >= 0x80000001u ? b->extended1Ecx : 0;
    if (!(ext & (1u << 5))) missing |= VORTEX_CPU_LZCNT;
    /* The AVX registers need the operating system's support: OSXSAVE, and XCR0 saving SSE and AVX state. */
    const int osAvx = (ecx & (1u << 27)) != 0 && (b->xcr0 & 6u) == 6u;
    if (!osAvx) missing |= VORTEX_CPU_AVX | VORTEX_CPU_AVX2 | VORTEX_CPU_FMA | VORTEX_CPU_F16C;
    return missing;
}

size_t vortex_cpu_names(unsigned mask, char* out, size_t size) {
    size_t length = 0;
    if (size == 0) return 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; ++i) {
        if (!(mask & kNames[i].bit)) continue;
        const int n = snprintf(out + length, size - length, "%s%s", length ? ", " : "", kNames[i].name);
        if (n < 0 || (size_t)n >= size - length) {
            out[size - 1] = '\0';
            return strlen(out);
        }
        length += (size_t)n;
    }
    return length;
}

unsigned vortex_cpu_parse_names(const char* list) {
    unsigned mask = 0;
    if (!list) return 0;
    while (*list) {
        while (*list == ',' || *list == ' ') ++list;
        const char* end = list;
        while (*end && *end != ',') ++end;
        for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; ++i) {
            const char* name = kNames[i].name;
            const size_t n = strlen(name);
            if ((size_t)(end - list) != n) continue;
            size_t k = 0;
            while (k < n && tolower((unsigned char)list[k]) == tolower((unsigned char)name[k])) ++k;
            if (k == n) mask |= kNames[i].bit;
        }
        list = end;
    }
    return mask;
}

#if VORTEX_CPU_X86
static void ReadCpu(VortexCpuBits* b) {
    memset(b, 0, sizeof *b);
#   if defined(__GNUC__) || defined(__clang__)
    unsigned a, bx, c, d;
    b->maxLeaf = __get_cpuid_max(0, NULL);
    b->maxExtendedLeaf = __get_cpuid_max(0x80000000u, NULL);
    if (b->maxLeaf >= 1 && __get_cpuid(1, &a, &bx, &c, &d)) b->leaf1Ecx = c;
    if (b->maxLeaf >= 7 && __get_cpuid_count(7, 0, &a, &bx, &c, &d)) b->leaf7Ebx = bx;
    if (b->maxExtendedLeaf >= 0x80000001u && __get_cpuid(0x80000001u, &a, &bx, &c, &d)) b->extended1Ecx = c;
    if (b->leaf1Ecx & (1u << 27)) {
        /* XGETBV only once OSXSAVE says the OS enabled it (it faults otherwise); inline, because the
         * _xgetbv intrinsic needs -mxsave. */
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        b->xcr0 = ((unsigned long long)hi << 32) | lo;
    }
#   else
    int r[4];
    __cpuid(r, 0);
    b->maxLeaf = (unsigned)r[0];
    __cpuid(r, (int)0x80000000);
    b->maxExtendedLeaf = (unsigned)r[0];
    if (b->maxLeaf >= 1) { __cpuid(r, 1); b->leaf1Ecx = (unsigned)r[2]; }
    if (b->maxLeaf >= 7) { __cpuidex(r, 7, 0); b->leaf7Ebx = (unsigned)r[1]; }
    if (b->maxExtendedLeaf >= 0x80000001u) { __cpuid(r, (int)0x80000001); b->extended1Ecx = (unsigned)r[2]; }
    if (b->leaf1Ecx & (1u << 27)) b->xcr0 = _xgetbv(0);
#   endif
}

static const char* ProgramName(void) {
#   if defined(__linux__)
    return program_invocation_short_name;
#   else
    return "VORTEX";
#   endif
}

static void VortexCpuCheck(void) {
    VortexCpuBits bits;
    ReadCpu(&bits);
    const unsigned missing = vortex_cpu_missing(&bits) | vortex_cpu_parse_names(getenv("VORTEX_CPU_CHECK_PRETEND_MISSING"));
    if (!missing) return;
    char names[128];
    vortex_cpu_names(missing, names, sizeof names);
    fprintf(stderr,
            "%s: this build needs an x86-64 CPU with SSE4.2, POPCNT, AVX, AVX2, FMA, BMI1, LZCNT and F16C "
            "(Intel Haswell 2013 / AMD Excavator 2015 or newer) and an OS that enables AVX; this CPU lacks: %s\n",
            ProgramName(), names);
    fflush(stderr);
    _Exit(4);
}

#   if defined(_MSC_VER)
#       pragma section(".CRT$XCT", read)
__declspec(allocate(".CRT$XCT")) void (*const vortex_cpu_check_entry)(void) = VortexCpuCheck;
#       if defined(_M_X64)
#           pragma comment(linker, "/include:vortex_cpu_check_entry")
#       else
#           pragma comment(linker, "/include:_vortex_cpu_check_entry")
#       endif
#   else
__attribute__((constructor(101))) static void VortexCpuCheckAtStart(void) { VortexCpuCheck(); }
#   endif
#endif

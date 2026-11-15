#pragma once

#include <cstdint>

#if defined(__aarch64__) || defined(_M_ARM64)
  #define PIANO_DENORMALS_FPCR 1
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
  #include <xmmintrin.h>
  #define PIANO_DENORMALS_MXCSR 1
#endif

namespace pm
{

/** Rounds denormal results to zero while in scope: decayed resonators and delay lines would otherwise crawl.
    The original sets the arm64 FPCR; Atlas builds for x64 too, where the same bits live in MXCSR. */
struct FlushDenormals
{
#if defined(PIANO_DENORMALS_FPCR)
    FlushDenormals() : saved (read()) { write (saved | (1ull << 24)); }
    ~FlushDenormals() { write (saved); }

  #if defined(_MSC_VER)
    static uint64_t read() { return (uint64_t) _ReadStatusReg (0x5A20); }       // ARM64_FPCR
    static void write (uint64_t v) { _WriteStatusReg (0x5A20, (__int64) v); }
  #else
    static uint64_t read() { uint64_t v; __asm__ volatile ("mrs %0, fpcr" : "=r" (v)); return v; }
    static void write (uint64_t v) { __asm__ volatile ("msr fpcr, %0" : : "r" (v)); }
  #endif

    uint64_t saved;
#elif defined(PIANO_DENORMALS_MXCSR)
    FlushDenormals() : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040); }   // flush to zero, denormals are zero
    ~FlushDenormals() { _mm_setcsr (saved); }

    unsigned int saved;
#else
    FlushDenormals() = default;
#endif
};

} // namespace pm

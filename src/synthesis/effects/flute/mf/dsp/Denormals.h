#pragma once
// Scoped flush-to-zero / denormals-are-zero. The engine enables this itself in
// process() instead of relying on the host. State is restored on scope exit.

#include <cstdint>
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define MF_DENORM_X86 1
#endif

namespace mf::dsp {

class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        std::uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        fpcr |= (std::uint64_t{1} << 24);  // FZ
        asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(MF_DENORM_X86)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u);  // FTZ | DAZ
#endif
    }
    ~ScopedFlushDenormals() {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        asm volatile("msr fpcr, %0" : : "r"(saved_));
#elif defined(MF_DENORM_X86)
        _mm_setcsr(static_cast<unsigned>(saved_));
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    [[maybe_unused]] std::uint64_t saved_ = 0;
};

} // namespace mf::dsp

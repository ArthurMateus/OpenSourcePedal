#pragma once

// RT-8 / C4: FTZ + DAZ control. This is the only place in dsp-core allowed to include <immintrin.h> (ADR-0006
// D3 "dsp-core/platform/ shim"). On non-x86 targets the functions are no-ops and report "flushed".

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define OSP_HAS_SSE_CSR 1
#else
#define OSP_HAS_SSE_CSR 0
#endif

namespace osp {

// True if both flush-to-zero and denormals-are-zero are on for the calling thread.
inline bool denormalsFlushed() noexcept
{
#if OSP_HAS_SSE_CSR
    return _MM_GET_FLUSH_ZERO_MODE() == _MM_FLUSH_ZERO_ON && _MM_GET_DENORMALS_ZERO_MODE() == _MM_DENORMALS_ZERO_ON;
#else
    return true;
#endif
}

// Sets FTZ + DAZ for its lifetime and restores the previous MXCSR afterwards. Used by dsp-core tests and
// osp-render; the app uses juce::ScopedNoDenormals.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept
    {
#if OSP_HAS_SSE_CSR
        previous_ = _mm_getcsr();
        _mm_setcsr(previous_ | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#endif
    }
    ~ScopedFlushDenormals()
    {
#if OSP_HAS_SSE_CSR
        _mm_setcsr(previous_);
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    unsigned int previous_ = 0;
};

} // namespace osp

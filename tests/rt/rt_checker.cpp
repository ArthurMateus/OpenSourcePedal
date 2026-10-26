// RT checker hooks (realtime-rules.md C2, C2b). Linked into osp_tests as an object library, so the
// replacement operator new/delete are the program's definitions. Each hook only counts when the calling
// thread is inside an osp::rt::ScopedAudioThread; it never allocates, locks or throws on the counting path.

#include "rt_checker.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

void* allocate(std::size_t size, std::size_t alignment) noexcept
{
    if (osp::rt::isAudioThread())
        osp::rt::reportAlloc();
    if (size == 0)
        size = 1;
#if defined(_MSC_VER)
    return alignment > alignof(std::max_align_t) ? _aligned_malloc(size, alignment) : std::malloc(size);
#else
    if (alignment <= alignof(std::max_align_t))
        return std::malloc(size);
    return std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
#endif
}

void deallocate(void* p, std::size_t alignment) noexcept
{
    if (p == nullptr)
        return;
    if (osp::rt::isAudioThread())
        osp::rt::reportFree();
#if defined(_MSC_VER)
    if (alignment > alignof(std::max_align_t)) {
        _aligned_free(p);
        return;
    }
#else
    (void)alignment;
#endif
    std::free(p);
}

void* allocateOrThrow(std::size_t size, std::size_t alignment)
{
    if (void* p = allocate(size, alignment))
        return p;
    throw std::bad_alloc();
}

constexpr std::size_t kDefault = alignof(std::max_align_t);

#if defined(_MSC_VER) && defined(_DEBUG)
// C2b: catches malloc/realloc/free from C code and third-party libraries. _CRT_BLOCK is the CRT's own
// bookkeeping.
int __cdecl crtAllocHook(int, void*, std::size_t, int blockType, long, const unsigned char*, int)
{
    if (blockType != _CRT_BLOCK && osp::rt::isAudioThread())
        osp::rt::reportCrtAlloc();
    return 1; // allow the operation; this hook only counts
}
const bool gCrtHookInstalled = (_CrtSetAllocHook(crtAllocHook), true);
#endif

} // namespace

// C2: every replaceable form (plain, array, nothrow, aligned, sized).
void* operator new(std::size_t n)
{
    return allocateOrThrow(n, kDefault);
}
void* operator new[](std::size_t n)
{
    return allocateOrThrow(n, kDefault);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    return allocate(n, kDefault);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    return allocate(n, kDefault);
}
void* operator new(std::size_t n, std::align_val_t a)
{
    return allocateOrThrow(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a)
{
    return allocateOrThrow(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept
{
    return allocate(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept
{
    return allocate(n, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept
{
    deallocate(p, kDefault);
}
void operator delete[](void* p) noexcept
{
    deallocate(p, kDefault);
}
void operator delete(void* p, std::size_t) noexcept
{
    deallocate(p, kDefault);
}
void operator delete[](void* p, std::size_t) noexcept
{
    deallocate(p, kDefault);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    deallocate(p, kDefault);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    deallocate(p, kDefault);
}
void operator delete(void* p, std::align_val_t a) noexcept
{
    deallocate(p, static_cast<std::size_t>(a));
}
void operator delete[](void* p, std::align_val_t a) noexcept
{
    deallocate(p, static_cast<std::size_t>(a));
}
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept
{
    deallocate(p, static_cast<std::size_t>(a));
}
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept
{
    deallocate(p, static_cast<std::size_t>(a));
}
void operator delete(void* p, std::align_val_t a, const std::nothrow_t&) noexcept
{
    deallocate(p, static_cast<std::size_t>(a));
}
void operator delete[](void* p, std::align_val_t a, const std::nothrow_t&) noexcept
{
    deallocate(p, static_cast<std::size_t>(a));
}

namespace osp::rt::test {

ViolationCounters operator-(const ViolationCounters& after, const ViolationCounters& before) noexcept
{
    return {after.allocs - before.allocs, after.frees - before.frees, after.crtAllocs - before.crtAllocs,
            after.locks - before.locks};
}

OutputScan scanOutput(std::span<const float> samples) noexcept
{
    OutputScan scan;
    for (const float x : samples) {
        const auto bits = std::bit_cast<std::uint32_t>(x);
        const std::uint32_t exponent = bits & 0x7F800000u;
        const std::uint32_t mantissa = bits & 0x007FFFFFu;
        if (exponent == 0x7F800000u)
            ++scan.nonFinite; // Inf or NaN
        else if (exponent == 0 && mantissa != 0)
            ++scan.subnormal;
    }
    return scan;
}

} // namespace osp::rt::test

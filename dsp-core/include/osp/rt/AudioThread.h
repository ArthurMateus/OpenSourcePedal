#pragma once

// Audio-thread scope and violation counters for the RT checker (realtime-rules.md C1, C5; ADR-0007
// REQ-TEST-006). Pure C++20: no JUCE, no OS headers. Always compiled (the cost is one thread_local read per
// check); the hooks that feed the counters (operator new/delete replacement, CRT alloc hook) live in tests/rt
// and are only linked into osp_tests and Debug app builds.

#include <atomic>
#include <cstdint>

// C6: lets the Linux Clang-20 RealtimeSanitizer job check the same functions. Empty on MSVC.
#if defined(__clang__) && __clang_major__ >= 20
#define OSP_RT_NONBLOCKING [[clang::nonblocking]]
#else
#define OSP_RT_NONBLOCKING
#endif

namespace osp::rt {

struct ViolationCounters {
    std::uint64_t allocs = 0;    // C2: operator new (all forms) on the audio thread
    std::uint64_t frees = 0;     // C2: operator delete (all forms) on the audio thread
    std::uint64_t crtAllocs = 0; // C2b: malloc/realloc/free seen by the MSVC Debug CRT hook
    std::uint64_t locks = 0;     // C3: osp::rt::Mutex::lock() on the audio thread

    friend bool operator==(const ViolationCounters&, const ViolationCounters&) = default;
};

// True while a ScopedAudioThread is alive on the calling thread.
bool isAudioThread() noexcept;

// C1: marks the calling thread as the audio thread for its lifetime. Nesting is allowed.
class ScopedAudioThread {
public:
    ScopedAudioThread() noexcept;
    ~ScopedAudioThread();
    ScopedAudioThread(const ScopedAudioThread&) = delete;
    ScopedAudioThread& operator=(const ScopedAudioThread&) = delete;

private:
    bool previous_;
};

// Snapshot of the process-wide counters. Compare a snapshot taken before and after an audio-thread scope.
ViolationCounters counters() noexcept;

// Called by the checker hooks and by osp::rt::Mutex. They never allocate, lock or throw.
void reportAlloc() noexcept;
void reportFree() noexcept;
void reportCrtAlloc() noexcept;
void reportLock() noexcept;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "RT-6: counters must be lock-free");

} // namespace osp::rt

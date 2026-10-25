#include "osp/rt/AudioThread.h"

namespace osp::rt {
namespace {

// Constant-initialised, so reading it never runs a dynamic TLS initialiser (safe inside operator new and CRT
// hooks).
thread_local bool tAudioThread = false;

std::atomic<std::uint64_t> gAllocs{0};
std::atomic<std::uint64_t> gFrees{0};
std::atomic<std::uint64_t> gCrtAllocs{0};
std::atomic<std::uint64_t> gLocks{0};

} // namespace

bool isAudioThread() noexcept
{
    return tAudioThread;
}

ScopedAudioThread::ScopedAudioThread() noexcept : previous_(tAudioThread)
{
    tAudioThread = true;
}

ScopedAudioThread::~ScopedAudioThread()
{
    tAudioThread = previous_;
}

ViolationCounters counters() noexcept
{
    ViolationCounters c;
    c.allocs = gAllocs.load(std::memory_order_relaxed);
    c.frees = gFrees.load(std::memory_order_relaxed);
    c.crtAllocs = gCrtAllocs.load(std::memory_order_relaxed);
    c.locks = gLocks.load(std::memory_order_relaxed);
    return c;
}

void reportAlloc() noexcept
{
    gAllocs.fetch_add(1, std::memory_order_relaxed);
}
void reportFree() noexcept
{
    gFrees.fetch_add(1, std::memory_order_relaxed);
}
void reportCrtAlloc() noexcept
{
    gCrtAllocs.fetch_add(1, std::memory_order_relaxed);
}
void reportLock() noexcept
{
    gLocks.fetch_add(1, std::memory_order_relaxed);
}

} // namespace osp::rt

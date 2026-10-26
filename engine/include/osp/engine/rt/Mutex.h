#pragma once

// C3: the only mutex type allowed in engine/ (realtime-rules.md RT-2). It is for non-RT threads only. Locking
// it on the audio thread counts a violation, which the RT checker turns into a test failure.

#include "osp/rt/AudioThread.h"

#include <mutex>

namespace osp::rt {

class Mutex {
public:
    void lock()
    {
        if (isAudioThread())
            reportLock();
        m_.lock();
    }
    // RT-2 exception: only with a fallback and review sign-off.
    bool try_lock() noexcept { return m_.try_lock(); }
    void unlock() noexcept { m_.unlock(); }

private:
    std::mutex m_;
};

} // namespace osp::rt

#pragma once

// Hands cab IR convolvers to the audio thread and takes the retired ones back (ADR-0004 §4). No JUCE.
//
// Control thread: install()/installNone() build the convolver (allocates) and queue it; collect() deletes what the
// graph has given back. Audio thread: pump() installs the queued requests, drain() moves every retired convolver that
// is ours to the retired queue (the engine's own "no cab" impulses stay with the engine). Both queues are
// single-producer single-consumer rings with no locks or allocation. Declare the installer before the Engine so that
// it outlives it: the engine's destructor lets go of installed convolvers without deleting them.

#include "osp/engine/Engine.h"
#include "osp/rt/AudioThread.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace osp::app {

class CabIrInstaller {
public:
    // Control thread. `ir` is already conditioned to the processing rate (IrLoader::condition). False when the queue
    // to the audio thread is full (nothing is installed, the caller may retry later).
    bool install(int channel, int slot, std::span<const float> ir);
    // Control thread. Slot 0 goes back to the engine's "no cab" impulse, any other slot to silence.
    bool installNone(osp::engine::Engine& engine, int channel, int slot);
    // Control thread. Deletes the convolvers the audio thread has retired.
    void collect();
    // Control thread. Convolvers we created that are queued or installed (for tests).
    std::size_t liveCount() const noexcept { return live_.size(); }

    // Audio thread.
    void pump(osp::engine::Engine& engine) noexcept OSP_RT_NONBLOCKING;
    void drain(osp::engine::Engine& engine) noexcept OSP_RT_NONBLOCKING;

private:
    static constexpr std::size_t kCapacity = 32; // a preset needs at most 6 requests

    struct Request {
        int channel = 0, slot = 0;
        osp::dsp::PartitionedConvolver* ir = nullptr;
    };

    template <class T>
    class Ring {
    public:
        bool push(const T& v) noexcept
        {
            const auto w = write_.load(std::memory_order_relaxed);
            if (w - read_.load(std::memory_order_acquire) == kCapacity)
                return false;
            items_[w % kCapacity] = v;
            write_.store(w + 1, std::memory_order_release);
            return true;
        }
        bool pop(T& v) noexcept
        {
            const auto r = read_.load(std::memory_order_relaxed);
            if (r == write_.load(std::memory_order_acquire))
                return false;
            v = items_[r % kCapacity];
            read_.store(r + 1, std::memory_order_release);
            return true;
        }

    private:
        std::array<T, kCapacity> items_{};
        std::atomic<std::size_t> write_{0}, read_{0};
    };

    Ring<Request> pending_;
    Ring<osp::dsp::PartitionedConvolver*> retired_;
    std::vector<std::unique_ptr<osp::dsp::PartitionedConvolver>> live_;
};

} // namespace osp::app

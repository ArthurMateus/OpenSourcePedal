// Self-tests: the RT checker must catch what it claims to catch (ADR-0007 REQ-TEST-006, realtime-rules.md
// C1–C4).

#include "osp/engine/rt/Mutex.h"
#include "rt_checker.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {
// Escapes the optimiser, so allocation pairs cannot be elided.
void* volatile gSink = nullptr;
} // namespace

using osp::rt::test::runAsAudioThread;

TEST_CASE("RT checker: catches operator new and delete on the audio thread", "[rt]")
{
    SECTION("new is counted, delete outside the scope is not")
    {
        int* p = nullptr;
        const auto r = runAsAudioThread([&] {
            p = new int(42);
            gSink = p;
        });
        delete p;
        CHECK(r.violations.allocs == 1u);
        CHECK(r.violations.frees == 0u);
    }
    SECTION("delete inside the scope is counted")
    {
        int* p = new int(42);
        gSink = p;
        const auto r = runAsAudioThread([&] { delete p; });
        CHECK(r.violations.allocs == 0u);
        CHECK(r.violations.frees == 1u);
    }
    SECTION("container growth is counted")
    {
        std::vector<float> v;
        const auto r = runAsAudioThread([&] { v.push_back(1.0f); });
        CHECK(r.violations.allocs >= 1u);
    }
    SECTION("over-aligned new is counted")
    {
        struct alignas(64) Wide {
            float x[16];
        };
        Wide* p = nullptr;
        const auto r = runAsAudioThread([&] {
            p = new Wide{};
            gSink = p;
        });
        delete p;
        CHECK(r.violations.allocs == 1u);
    }
    SECTION("allocations on a non-audio thread are not counted")
    {
        const auto before = osp::rt::counters();
        int* p = new int(1);
        gSink = p;
        delete p;
        CHECK(osp::rt::counters() == before);
    }
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("RT checker: the Debug CRT hook catches malloc and free (C2b)", "[rt]")
{
    void* p = nullptr;
    const auto r = runAsAudioThread([&] {
        p = std::malloc(16);
        gSink = p;
    });
    std::free(p);
    CHECK(r.violations.crtAllocs >= 1u);
    CHECK(r.violations.allocs == 0u);
}
#endif

TEST_CASE("RT checker: catches osp::rt::Mutex::lock on the audio thread (C3)", "[rt]")
{
    osp::rt::Mutex m;
    const auto before = osp::rt::counters();
    m.lock(); // a control thread may lock
    m.unlock();
    CHECK(osp::rt::counters() == before);

    const auto r = runAsAudioThread([&] {
        m.lock();
        m.unlock();
    });
    CHECK(r.violations.locks == 1u);

    const auto t = runAsAudioThread([&] {
        if (m.try_lock())
            m.unlock();
    });
    CHECK(t.violations.locks == 0u);
}

TEST_CASE("RT checker: the audio-thread scope nests and restores (C1)", "[rt]")
{
    REQUIRE_FALSE(osp::rt::isAudioThread());
    {
        osp::rt::ScopedAudioThread outer;
        {
            osp::rt::ScopedAudioThread inner;
            CHECK(osp::rt::isAudioThread());
        }
        CHECK(osp::rt::isAudioThread());
    }
    CHECK_FALSE(osp::rt::isAudioThread());
}

TEST_CASE("RT checker: FTZ/DAZ are on inside the scope and restored after (C4)", "[rt]")
{
    const bool before = osp::denormalsFlushed();
    const auto r = runAsAudioThread([] {});
    CHECK(r.denormalsFlushed);
    CHECK(osp::denormalsFlushed() == before);
    OSP_REQUIRE_RT_CLEAN(r);
}

TEST_CASE("RT checker: the output scan catches NaN, Inf and subnormal samples (C4, RT-9)", "[rt]")
{
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    constexpr float kSub = std::numeric_limits<float>::denorm_min();

    const std::array<float, 4> clean{0.0f, -0.0f, 1.0f, std::numeric_limits<float>::min()};
    CHECK(osp::rt::test::scanOutput(clean).clean());

    const std::array<float, 6> dirty{0.5f, kNaN, kInf, -kInf, kSub, -kSub};
    const auto scan = osp::rt::test::scanOutput(dirty);
    CHECK(scan.nonFinite == 3u);
    CHECK(scan.subnormal == 2u);
    CHECK_FALSE(scan.clean());
}

// Issue #198: the engine's lock-free input and output peaks, read by the editor's meters.

#include "osp/engine/Engine.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using namespace osp::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE("Engine meters: the input peak is the largest host input sample since the last read", "[engine][meters]")
{
    Engine engine;
    engine.prepare(48000.0, 256);

    std::vector<float> left(256, 0.0f), right(256, 0.0f);
    left[10] = -0.5f;
    left[200] = 0.25f;
    float* channels[2] = {left.data(), right.data()};
    engine.process(channels, 2, 256);

    CHECK_THAT(engine.takeInputPeak(), WithinAbs(0.5, 1.0e-6));
    CHECK(engine.takeInputPeak() == 0.0f); // cleared by the read
}

TEST_CASE("Engine meters: the output peak follows the processed signal and keeps the largest block", "[engine][meters]")
{
    Engine engine;
    engine.prepare(48000.0, 256);

    std::vector<float> left(256), right(256);
    float* channels[2] = {left.data(), right.data()};
    float largest = 0.0f;
    for (int block = 0; block < 40; ++block) {
        for (int n = 0; n < 256; ++n)
            left[static_cast<std::size_t>(n)] = 0.3f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f *
                                                                static_cast<float>(block * 256 + n) / 48000.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        engine.process(channels, 2, 256);
        for (std::size_t n = 0; n < 256; ++n)
            largest = std::max({largest, std::abs(left[n]), std::abs(right[n])});
    }
    CHECK_THAT(engine.takeOutputPeak(), WithinAbs(largest, 1.0e-6));
    CHECK(engine.takeOutputPeak() == 0.0f);
}

TEST_CASE("Engine meters: updating the peaks allocates nothing on the audio thread", "[engine][meters][rt]")
{
    Engine engine;
    engine.prepare(48000.0, 128);
    std::vector<float> left(128, 0.1f), right(128, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    const auto rt = osp::rt::test::runAsAudioThread([&] { engine.process(channels, 2, 128); });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(engine.takeInputPeak() > 0.0f);
}

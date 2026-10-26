#include "osp/engine/Chain.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

TEST_CASE("Chain: pass-through at 0 dB is bit-identical for every rate x block size", "[engine][chain]")
{
    const double rate = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(1, 32, 63, 64, 512, 2048);

    std::vector<float> in(5000);
    for (std::size_t n = 0; n < in.size(); ++n)
        in[n] = 0.5f * std::sin(0.013f * static_cast<float>(n));

    osp::engine::Chain chain;
    chain.prepare(rate, 2048);
    std::vector<float> out(in.size(), -1.0f);
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos < in.size(); pos += static_cast<std::size_t>(block)) {
            const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), in.size() - pos));
            chain.setOutputLevelDb(0.0f);
            chain.process(in.data() + pos, out.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    CHECK(out == in);
    CHECK(chain.getLatencySamples() == 0);
}

TEST_CASE("Chain: output level settles to the target after the ramp", "[engine][chain]")
{
    osp::engine::Chain chain;
    chain.prepare(48000.0, 64);
    chain.setOutputLevelDb(-6.0f);
    std::vector<float> buf(4800, 1.0f); // 100 ms, longer than the 20 ms ramp
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos < buf.size(); pos += 64)
            chain.process(buf.data() + pos, buf.data() + pos,
                          static_cast<int>(std::min<std::size_t>(64, buf.size() - pos)));
    }));
    OSP_REQUIRE_OUTPUT_CLEAN(buf);
    CHECK(std::abs(buf.back() - std::pow(10.0f, -6.0f / 20.0f)) < 1e-6f);
}

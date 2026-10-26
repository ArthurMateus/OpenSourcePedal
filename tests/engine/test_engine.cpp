#include "osp/engine/Engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace {

std::vector<float> ramp(int length, float offset) {
  std::vector<float> x(static_cast<std::size_t>(length));
  for (int n = 0; n < length; ++n)
    x[static_cast<std::size_t>(n)] =
        offset + 0.001f * static_cast<float>(n % 500);
  return x;
}

} // namespace

TEST_CASE("Engine: pass-through across rates and host block sizes, incl. 2 x "
          "maxBlock",
          "[engine]") {
  // ADR-0007 REQ-TEST-001: maxBlockSize 2048, and the app boundary also gets 2
  // x maxBlock.
  const double sampleRate = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
  const int hostBlock = GENERATE(0, 1, 32, 64, 127, 512, 2048, 4096);
  CAPTURE(sampleRate, hostBlock);

  std::atomic<float> gainDb{0.0f};
  osp::engine::Engine engine;
  engine.setGainParameter(&gainDb);
  engine.prepare(sampleRate, 2048);
  REQUIRE(engine.getLatencySamples() == 0);

  auto left = ramp(9000, 0.1f), right = ramp(9000, -0.2f);
  const auto inLeft = left, inRight = right;
  const int step = hostBlock > 0 ? hostBlock : 64;
  for (int pos = 0; pos < 9000; pos += step) {
    float *channels[] = {left.data() + pos, right.data() + pos};
    if (hostBlock == 0)
      osp::engine::processChunked(engine, channels, 2, 0);
    osp::engine::processChunked(engine, channels, 2,
                                std::min(step, 9000 - pos));
  }
  REQUIRE(left == inLeft);
  REQUIRE(right == inRight);
}

TEST_CASE("Engine: reads the gain atomic once per block and ramps to it",
          "[engine]") {
  std::atomic<float> gainDb{0.0f};
  osp::engine::Engine engine;
  engine.setGainParameter(&gainDb);
  engine.prepare(48000.0, 64);

  gainDb.store(-6.0206f);
  std::vector<float> x(4800, 1.0f); // 100 ms, longer than the 20 ms ramp
  float *channels[] = {x.data()};
  osp::engine::processChunked(engine, channels, 1, 4800);
  REQUIRE(x.front() < 1.0f);
  REQUIRE(x.front() > 0.99f);
  REQUIRE(std::abs(x.back() - 0.5f) < 1e-4f);
}

TEST_CASE("Engine: process() before prepare() zero-fills", "[engine]") {
  osp::engine::Engine engine;
  std::vector<float> x(32, 1.0f);
  float *channels[] = {x.data()};
  osp::engine::processChunked(engine, channels, 1, 32);
  for (float v : x)
    REQUIRE(v == 0.0f);
}

#include "osp/engine/Params.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <limits>
#include <set>
#include <string>

using osp::engine::findParam;
using osp::engine::ParamKind;
using osp::engine::ParamStore;
using osp::engine::paramTable;
using osp::engine::Skew;
using osp::engine::Visibility;

namespace {

const osp::engine::ParamInfo& info(std::string_view id)
{
    const int i = findParam(id);
    REQUIRE(i >= 0);
    return paramTable()[static_cast<std::size_t>(i)];
}

} // namespace

TEST_CASE("Params: 226 host-visible ids, unique, defaults in range (parameters.md section 5)", "[engine][params]")
{
    CHECK(osp::engine::hostParamCount() == 226);
    std::set<std::string> ids;
    for (const auto& p : paramTable()) {
        INFO(p.id);
        CHECK(ids.insert(p.id).second);
        CHECK(p.min <= p.def);
        CHECK(p.def <= p.max);
        CHECK(osp::engine::sanitize(p, p.def) == p.def);
        if (p.skew == Skew::log)
            CHECK(p.min > 0.0f);
        if (p.kind == ParamKind::choice)
            CHECK(static_cast<float>(p.choices.size()) == p.max + 1.0f);
        else
            CHECK(p.choices.empty());
    }
    CHECK(findParam("no.such.id") == -1);
}

TEST_CASE("Params: per-channel and preset-only entries from parameters.md", "[engine][params]")
{
    CHECK(info("amp.ch1.gain").def == 3.0f);
    CHECK(info("amp.ch2.gain").def == 5.0f);
    CHECK(info("amp.ch3.gain").def == 6.0f);
    CHECK(info("cab.ch3.ir2.enabled").def == 0.0f);
    CHECK(info("cab.ch3.ir1.enabled").def == 1.0f);
    CHECK(info("eq.ch2.p3.freqHz").def == 2000.0f);
    CHECK(info("eq.ch2.g16kDb").vis == Visibility::host);
    CHECK(info("eq.ch1.mode").vis == Visibility::preset);
    CHECK(info("mod.position").vis == Visibility::preset);
    CHECK(info("gate.detector").vis == Visibility::preset);
    CHECK(info("amp.cabLink").vis == Visibility::preset);
    CHECK(info("transpose.semitones").vis == Visibility::reserved);
    CHECK(info("delay.sync").choices.size() == 22);
    CHECK(info("delay.sync").choices[14] == "1/4"); // dsp::SyncDivision::n1_4
    CHECK(info("mod.type").choices[static_cast<std::size_t>(info("mod.type").def)] == "chorusWide");
    CHECK(info("reverb.mode").choices[static_cast<std::size_t>(info("reverb.mode").def)] == "hall");
    // Every block that has an enable flag (parameters.md §1.1, §1.6): amp and output have none.
    for (const char* block :
         {"input", "gate", "comp", "drive1", "drive2", "cab", "eq", "mod", "delay", "reverb", "doubler"})
        CHECK(findParam(std::string(block) + ".enabled") >= 0);
    CHECK(findParam("amp.enabled") == -1);
    CHECK(findParam("output.enabled") == -1);
}

TEST_CASE("Params: normalized <-> plain round trip to 1e-6 relative for every entry (REQ-PAR-003)", "[engine][params]")
{
    for (const auto& p : paramTable()) {
        INFO(p.id);
        for (int k = 0; k <= 16; ++k) {
            const float plain = osp::engine::fromNormalized(p, static_cast<float>(k) / 16.0f);
            const float back = osp::engine::fromNormalized(p, osp::engine::toNormalized(p, plain));
            const float tol = 1e-6f * std::max(1.0f, std::abs(plain));
            CHECK(std::abs(back - plain) <= tol);
        }
        CHECK(osp::engine::fromNormalized(p, 0.0f) == p.min);
        CHECK(std::abs(osp::engine::fromNormalized(p, 1.0f) - p.max) <= 1e-6f * std::max(1.0f, std::abs(p.max)));
    }
}

TEST_CASE("Params: sanitize clamps, snaps and maps NaN to the minimum", "[engine][params]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(osp::engine::sanitize(info("gate.thresholdDb"), nan) == -90.0f);
    CHECK(osp::engine::sanitize(info("gate.thresholdDb"), inf) == -20.0f);
    CHECK(osp::engine::sanitize(info("comp.mode"), 1.4f) == 1.0f);
    CHECK(osp::engine::sanitize(info("comp.mode"), 7.0f) == 2.0f);
    CHECK(osp::engine::sanitize(info("gate.postAmp"), 0.7f) == 1.0f);
    CHECK(osp::engine::sanitize(info("transpose.semitones"), -3.6f) == -4.0f);
}

TEST_CASE("ParamStore: defaults, set by id, bind to an external atomic", "[engine][params]")
{
    ParamStore store;
    REQUIRE(store.size() == static_cast<int>(paramTable().size()));
    const int gain = findParam("input.gainDb");
    CHECK(store.get(gain) == 0.0f);
    CHECK(store.set("input.gainDb", 30.0f)); // clamped
    CHECK(store.get(gain) == 24.0f);
    CHECK_FALSE(store.set("input.nope", 1.0f));

    std::atomic<float> host{-6.0f};
    store.bind(gain, &host);
    CHECK(store.get(gain) == -6.0f);
    host.store(3.0f);
    CHECK(store.get(gain) == 3.0f);
    store.bind(gain, nullptr);
    CHECK(store.get(gain) == 24.0f);
    CHECK(store.get(-1) == 0.0f);
    CHECK(store.get(store.size()) == 0.0f);
}

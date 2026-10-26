#include "osp/engine/Version.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("engine reports the CMake project version", "[engine]") {
  REQUIRE(osp::engine::versionString() == "0.1.0");
}

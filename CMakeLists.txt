# OpenSourcePedal: top-level build. Layout, options and flags follow ADR-0006 (docs/adr/0006-build-and-dependencies.md).
cmake_minimum_required(VERSION 3.25)

project(OpenSourcePedal VERSION 0.1.0 LANGUAGES C CXX) # C: JUCE 9 and libebur128 (tools/)

# --- Options (ADR-0006 D4) ---------------------------------------------------------------------------------------
option(OSP_BUILD_APP "Fetch JUCE and build app/ + ui/" ON)
option(OSP_BUILD_TESTS "Build tests/ and osp-bench" ON)
option(OSP_BUILD_TOOLS "Build tools/ (osp-render)" ON)
option(OSP_ENABLE_ASIO "JUCE_ASIO=1 (headers bundled in JUCE)" ON)
option(OSP_RT_CHECKER "Compile the audio-thread allocation/lock checker" OFF)
option(OSP_ENABLE_AVX2 "Compile with /arch:AVX2 (baseline is SSE2)" OFF)
option(OSP_WARNINGS_AS_ERRORS "Treat warnings as errors on our targets" OFF)

# --- Global settings (ADR-0006 D6), set before any dependency is fetched -----------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT for every target, ours and third-party.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Debug info in every configuration (separate PDB), so CI crash dumps symbolise.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OspCompilerSettings)
include(OspDependencies)

add_subdirectory(dsp-core)
add_subdirectory(engine)

if(OSP_BUILD_APP)
    add_subdirectory(app)
endif()

if(OSP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OSP_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# Every third-party dependency, pinned by full commit SHA (git) or SHA-256 (archives). ADR-0006 D1/D2.
# A dependency is added here only in the PR that first uses it, together with its THIRD_PARTY.md row.
include_guard(GLOBAL)
include(FetchContent)

# HIIR 1.50 (WTFPL-2.0), header-only, used by dsp-core's IIR oversampler (ADR-0005 D1, ADR-0006 D2). There is no
# repository, only this archive, so it is pinned by SHA-256 (checked against ADR-0006 on 2026-10-26). The archive has
# no CMakeLists.txt: SOURCE_SUBDIR keeps FetchContent to a download. The second URL is a byte-identical mirror on this
# repo's `deps-hiir-1.50` pre-release (#99). CMake tries the URLs in order, and the hash is checked whichever one serves.
FetchContent_Declare(hiir
    URL      http://ldesoras.fr/src/hiir-1.50.zip
             https://github.com/ArthurMateus/OpenSourcePedal/releases/download/deps-hiir-1.50/hiir-1.50.zip
    URL_HASH SHA256=8a13724e8d698a40f53687d40466443879f6250abd2229097f3990b76e6855a6
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR osp-no-build)
FetchContent_MakeAvailable(hiir)

# Signalsmith dsp v1.7.3 (MIT), header-only. Its FFT backs the cab convolution in dsp-core (#69) and tests/measure
# (ADR-0006 Amendment A1, #62). Tag → commit checked 2026-10-25.
FetchContent_Declare(signalsmith_dsp
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/dsp.git
    GIT_TAG        ca390d28c41ea5108a528e2e81faa4088f2f8ac8 # v1.7.3
    GIT_SHALLOW    OFF
    SYSTEM)
FetchContent_MakeAvailable(signalsmith_dsp)

if(OSP_BUILD_APP)
    # JUCE 9.0.4 (AGPLv3 option of the dual licence). Bundles the VST3 SDK (MIT) and ASIO headers (GPL-3.0). ADR-0006 D2.
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        3fb5cbc0340758596dac39f7c8de47d562b4896e # 9.0.4
        GIT_SHALLOW    OFF
        SYSTEM)
    FetchContent_MakeAvailable(JUCE)
endif()

if(OSP_BUILD_TESTS)
    # Catch2 v3.16.1 (BSL-1.0). Tag object 5010924c… dereferences to this commit (checked 2026-10-25).
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        08092139210881f01a34e60c5f2fd6afa0ea4024 # v3.16.1
        GIT_SHALLOW    OFF
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras") # for include(Catch)

endif()

# Header-only or C sources that we compile ourselves (tests/bench, tools/CMakeLists.txt). SOURCE_SUBDIR points at a
# directory without a CMakeLists.txt, so FetchContent only downloads them and never runs their own build/install rules.
if(OSP_BUILD_TESTS)
    # nanobench v4.6.0 (MIT). Lightweight tag, commit checked via the GitHub API on 2026-10-25.
    FetchContent_Declare(nanobench
        GIT_REPOSITORY https://github.com/martinus/nanobench.git
        GIT_TAG        b709728dba4df0356d467abfb0ba20781a8031c0 # v4.6.0
        GIT_SHALLOW    OFF
        SOURCE_SUBDIR  osp-no-build)
    FetchContent_MakeAvailable(nanobench)
endif()

if(OSP_BUILD_TOOLS)
    # libebur128 v1.2.6 (MIT). Lightweight tag, commit checked via the GitHub API on 2026-10-25.
    FetchContent_Declare(libebur128
        GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
        GIT_TAG        67b33abe1558160ed76ada1322329b0e9e058b02 # v1.2.6
        GIT_SHALLOW    OFF
        SOURCE_SUBDIR  osp-no-build)
    FetchContent_MakeAvailable(libebur128)
endif()

# dr_wav reads WAV files for tools/osp-render and for the tests that load the CC0 assets (#69). One shared target.
if(OSP_BUILD_TESTS OR OSP_BUILD_TOOLS)
    # dr_libs (dr_wav 0.14.6, Unlicense OR MIT-0). dr_libs has no per-library tags. This commit's dr_wav.h reports
    # 0.14.6 (DRWAV_VERSION_*, checked 2026-10-25).
    FetchContent_Declare(dr_libs
        GIT_REPOSITORY https://github.com/mackron/dr_libs.git
        GIT_TAG        14423ee630e4b36596d042fa055d7f1d0a02ca38 # dr_wav 0.14.6
        GIT_SHALLOW    OFF
        SOURCE_SUBDIR  osp-no-build)
    FetchContent_MakeAvailable(dr_libs)
    add_library(osp_dr_wav STATIC "${PROJECT_SOURCE_DIR}/tools/render/dr_wav_impl.c")
    target_include_directories(osp_dr_wav SYSTEM PUBLIC "${dr_libs_SOURCE_DIR}")
    target_compile_definitions(osp_dr_wav PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

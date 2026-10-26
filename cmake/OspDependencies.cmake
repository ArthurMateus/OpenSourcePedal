# Every third-party dependency, pinned by full commit SHA (git) or SHA-256 (archives). ADR-0006 D1/D2.
# A dependency is added here only in the PR that first uses it, together with its THIRD_PARTY.md row.
include_guard(GLOBAL)
include(FetchContent)

if(OSP_BUILD_TESTS)
    # Catch2 v3.16.1 (BSL-1.0). Tag object 5010924c… dereferences to this commit (checked 2026-10-25).
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        08092139210881f01a34e60c5f2fd6afa0ea4024 # v3.16.1
        GIT_SHALLOW    OFF
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras") # for include(Catch)

    # Signalsmith dsp v1.7.3 (MIT), header-only; its FFT backs tests/measure (ADR-0006 D2 amendment, #62).
    # Tag → commit checked 2026-10-25. It moves out of this block when the cab IR convolution first uses it.
    FetchContent_Declare(signalsmith_dsp
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/dsp.git
        GIT_TAG        ca390d28c41ea5108a528e2e81faa4088f2f8ac8 # v1.7.3
        GIT_SHALLOW    OFF
        SYSTEM)
    FetchContent_MakeAvailable(signalsmith_dsp)
endif()

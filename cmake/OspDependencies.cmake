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
endif()

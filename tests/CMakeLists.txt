add_executable(osp_tests
    dsp/test_gain.cpp
    engine/test_chain.cpp
    engine/test_version.cpp)

target_link_libraries(osp_tests PRIVATE osp_engine Catch2::Catch2WithMain)
osp_apply_settings(osp_tests)
# The RT checker is always compiled into the test binary (ADR-0006 D4).
target_compile_definitions(osp_tests PRIVATE OSP_RT_CHECKER=1)

include(Catch)
catch_discover_tests(osp_tests DISCOVERY_MODE PRE_TEST)

add_subdirectory(bench)

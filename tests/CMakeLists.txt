# RT checker hooks (realtime-rules.md §2, ADR-0007 REQ-TEST-006). An object library, so the replacement
# operator new/delete are linked into the test program itself.
add_library(osp_rt_checker OBJECT
    rt/rt_checker.cpp)
target_include_directories(osp_rt_checker PUBLIC rt)
target_link_libraries(osp_rt_checker PUBLIC osp_dsp_core)
osp_apply_settings(osp_rt_checker)

add_executable(osp_tests
    dsp/test_gain.cpp
    engine/test_chain.cpp
    engine/test_engine.cpp
    engine/test_version.cpp
    rt/test_rt_checker.cpp)

target_link_libraries(osp_tests PRIVATE osp_engine osp_rt_checker Catch2::Catch2WithMain)
osp_apply_settings(osp_tests)
# The RT checker is always compiled into the test binary (ADR-0006 D4).
target_compile_definitions(osp_tests PRIVATE OSP_RT_CHECKER=1)

include(Catch)
catch_discover_tests(osp_tests DISCOVERY_MODE PRE_TEST)

add_subdirectory(bench)

# RT checker hooks (realtime-rules.md §2, ADR-0007 REQ-TEST-006). An object library, so the replacement
# operator new/delete are linked into the test program itself.
add_library(osp_rt_checker OBJECT
    rt/rt_checker.cpp)
target_include_directories(osp_rt_checker PUBLIC rt)
target_link_libraries(osp_rt_checker PUBLIC osp_dsp_core)
osp_apply_settings(osp_rt_checker)

# Measurement harness (ADR-0007 §2, §3): FFT, coherent THD/ASR, smoothing residual, null, ESS, LSD/MR-STFT.
add_library(osp_measure STATIC
    measure/measure.cpp
    measure/spectral.cpp)
target_include_directories(osp_measure PUBLIC measure)
target_link_libraries(osp_measure PRIVATE signalsmith-dsp)
osp_apply_settings(osp_measure)

# Every tests/<area>/test_*.cpp is part of osp_tests; adding a block adds a file, not a line here (#115).
# tests/app (needs JUCE) and tests/bench are built separately.
file(GLOB OSP_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/dsp/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/measure/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rt/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_*.cpp")
add_executable(osp_tests ${OSP_TEST_SOURCES})

# tools/tonematch/Sha256.h is header-only and tested here (ADR-0008).
target_include_directories(osp_tests PRIVATE "${PROJECT_SOURCE_DIR}/tools/tonematch")
target_link_libraries(osp_tests PRIVATE osp_engine osp_measure osp_rt_checker osp_dr_wav Catch2::Catch2WithMain)
# The asset tests read the bundled CC0 IRs and DI clips (Git LFS; CI checks them out with lfs: true).
target_compile_definitions(osp_tests PRIVATE OSP_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
osp_apply_settings(osp_tests)
# The RT checker is always compiled into the test binary (ADR-0006 D4).
target_compile_definitions(osp_tests PRIVATE OSP_RT_CHECKER=1)

include(Catch)
catch_discover_tests(osp_tests DISCOVERY_MODE PRE_TEST)

add_subdirectory(bench)

# App-level tests link the plugin's shared-code target (JUCE), so they exist only with OSP_BUILD_APP.
if(OSP_BUILD_APP)
    add_executable(osp_app_tests app/test_state_roundtrip.cpp)
    # JUCE symbols come from the OpenSourcePedal library; only the module headers are needed here (as SYSTEM, so our
    # /W4 /WX does not apply to them). Linking juce:: module targets again would compile JUCE a second time.
    target_include_directories(osp_app_tests PRIVATE "${PROJECT_SOURCE_DIR}/app/src")
    target_include_directories(osp_app_tests SYSTEM PRIVATE "${JUCE_MODULES_DIR}")
    target_link_libraries(osp_app_tests PRIVATE OpenSourcePedal Catch2::Catch2WithMain)
    osp_apply_settings(osp_app_tests WARNINGS_ON app/test_state_roundtrip.cpp)
    catch_discover_tests(osp_app_tests DISCOVERY_MODE PRE_TEST)
endif()

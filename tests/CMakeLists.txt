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

add_executable(osp_tests
    dsp/test_adaa.cpp
    dsp/test_cab.cpp
    dsp/test_chorus_flanger.cpp
    dsp/test_compressor.cpp
    dsp/test_convolver.cpp
    dsp/test_eq.cpp
    dsp/test_eq_dynamics.cpp
    dsp/test_svf.cpp
    dsp/test_gain.cpp
    dsp/test_gate.cpp
    dsp/test_ir_loader.cpp
    dsp/test_oversampler.cpp
    dsp/test_phaser.cpp
    dsp/test_tremolo.cpp
    dsp/test_util.cpp
    engine/test_chain.cpp
    engine/test_engine.cpp
    engine/test_version.cpp
    measure/test_measure.cpp
    measure/test_spectral.cpp
    rt/test_rt_checker.cpp)

target_link_libraries(osp_tests PRIVATE osp_engine osp_measure osp_rt_checker osp_dr_wav Catch2::Catch2WithMain)
# The asset tests read the bundled CC0 IRs and DI clips (Git LFS; CI checks them out with lfs: true).
target_compile_definitions(osp_tests PRIVATE OSP_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets")
osp_apply_settings(osp_tests)
# The RT checker is always compiled into the test binary (ADR-0006 D4).
target_compile_definitions(osp_tests PRIVATE OSP_RT_CHECKER=1)

include(Catch)
catch_discover_tests(osp_tests DISCOVERY_MODE PRE_TEST)

add_subdirectory(bench)

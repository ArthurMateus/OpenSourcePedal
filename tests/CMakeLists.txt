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
    dsp/test_cab_req.cpp
    dsp/test_chorus_flanger.cpp
    dsp/test_compressor.cpp
    dsp/test_convolver.cpp
    dsp/test_delay.cpp
    dsp/test_delay_voice.cpp
    dsp/test_delay_voicings.cpp
    dsp/test_drive.cpp
    dsp/test_drive_region.cpp
    dsp/test_doubler.cpp
    dsp/test_eq.cpp
    dsp/test_fdn_tank.cpp
    dsp/test_plate_tank.cpp
    dsp/test_spring_tank.cpp
    dsp/test_reverb.cpp
    dsp/test_fractional_delay.cpp
    dsp/test_eq_dynamics.cpp
    dsp/test_fdn_tank.cpp
    dsp/test_svf.cpp
    dsp/test_tuner.cpp
    dsp/test_tuner_tap.cpp
    dsp/test_gain.cpp
    dsp/test_gate.cpp
    dsp/test_input_stage.cpp
    dsp/test_ir_loader.cpp
    dsp/test_modulation.cpp
    dsp/test_oversampler.cpp
    dsp/test_output_stage.cpp
    dsp/test_phaser.cpp
    dsp/test_tremolo.cpp
    dsp/test_util.cpp
    engine/test_amp_slot.cpp
    engine/test_chain.cpp
    engine/test_engine.cpp
    engine/test_graph.cpp
    engine/test_graph_gate.cpp
    engine/test_graph_latency.cpp
    engine/test_params.cpp
    engine/test_state.cpp
    engine/test_tone_profile.cpp
    engine/test_tone_sections.cpp
    engine/test_tonematch.cpp
    engine/test_version.cpp
    measure/test_measure.cpp
    measure/test_spectral.cpp
    rt/test_rt_checker.cpp
    tools/test_sha256.cpp)

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

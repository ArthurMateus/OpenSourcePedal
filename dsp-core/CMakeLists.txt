add_library(osp_dsp_core STATIC
    src/Cab.cpp
    src/ChorusFlanger.cpp
    src/Compressor.cpp
    src/conv/IrLoader.cpp
    src/conv/PartitionedConvolver.cpp
    src/Eq.cpp
    src/Gain.cpp
    src/Gate.cpp
    src/InputStage.cpp
    src/Modulation.cpp
    src/oversampling/Oversampler.cpp
    src/Phaser.cpp
    src/Tremolo.cpp
    src/util/DelayLine.cpp
    src/rt/AudioThread.cpp)
add_library(osp::dsp_core ALIAS osp_dsp_core)

target_include_directories(osp_dsp_core PUBLIC include)
# HIIR is PRIVATE: no HIIR type appears in a public header (ADR-0006 D3).
target_include_directories(osp_dsp_core SYSTEM PRIVATE "${hiir_SOURCE_DIR}")
# Header-only FFT, used only inside src/ (ADR-0006 Amendment A1).
target_link_libraries(osp_dsp_core PRIVATE signalsmith-dsp)
target_compile_features(osp_dsp_core PUBLIC cxx_std_20)
osp_apply_settings(osp_dsp_core)

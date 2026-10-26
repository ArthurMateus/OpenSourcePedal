add_library(osp_dsp_core STATIC
    src/Compressor.cpp
    src/Drive.cpp
    src/DriveRegion.cpp
    src/Gain.cpp
    src/oversampling/Oversampler.cpp
    src/util/DelayLine.cpp
    src/rt/AudioThread.cpp)
add_library(osp::dsp_core ALIAS osp_dsp_core)

target_include_directories(osp_dsp_core PUBLIC include)
# HIIR is PRIVATE: no HIIR type appears in a public header (ADR-0006 D3).
target_include_directories(osp_dsp_core SYSTEM PRIVATE "${hiir_SOURCE_DIR}")
target_compile_features(osp_dsp_core PUBLIC cxx_std_20)
osp_apply_settings(osp_dsp_core)

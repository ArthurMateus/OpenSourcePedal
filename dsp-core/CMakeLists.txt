add_library(osp_dsp_core STATIC
    src/Compressor.cpp
    src/Gain.cpp
    src/Reverb.cpp
    src/reverb/FdnTank.cpp
    src/util/DelayLine.cpp
    src/rt/AudioThread.cpp)
add_library(osp::dsp_core ALIAS osp_dsp_core)

target_include_directories(osp_dsp_core PUBLIC include)
target_compile_features(osp_dsp_core PUBLIC cxx_std_20)
osp_apply_settings(osp_dsp_core)

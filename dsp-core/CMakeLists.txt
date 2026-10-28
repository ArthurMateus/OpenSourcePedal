# Every .cpp under src/ is part of the library; adding a block adds a file, not a line here (#115).
file(GLOB_RECURSE OSP_DSP_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(osp_dsp_core STATIC ${OSP_DSP_CORE_SOURCES})
add_library(osp::dsp_core ALIAS osp_dsp_core)

target_include_directories(osp_dsp_core PUBLIC include)
# HIIR is PRIVATE: no HIIR type appears in a public header (ADR-0006 D3).
target_include_directories(osp_dsp_core SYSTEM PRIVATE "${hiir_SOURCE_DIR}")
# Header-only FFT, used only inside src/ (ADR-0006 Amendment A1).
target_link_libraries(osp_dsp_core PRIVATE signalsmith-dsp)
target_compile_features(osp_dsp_core PUBLIC cxx_std_20)
osp_apply_settings(osp_dsp_core)

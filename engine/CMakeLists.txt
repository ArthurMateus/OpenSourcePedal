add_library(osp_engine STATIC
    src/Chain.cpp
    src/Engine.cpp
    src/Params.cpp
    src/State.cpp
    src/ToneMatch.cpp
    src/ToneProfile.cpp
    src/ToneSections.cpp
    src/Version.cpp
    src/graph/AmpSlot.cpp)
add_library(osp::engine ALIAS osp_engine)

target_include_directories(osp_engine PUBLIC include)
target_link_libraries(osp_engine PUBLIC osp_dsp_core PRIVATE nlohmann_json::nlohmann_json)
# Header-only FFT for the tone-match analysis, used only inside src/ (ADR-0008).
target_link_libraries(osp_engine PRIVATE signalsmith-dsp)
target_compile_definitions(osp_engine PRIVATE OSP_VERSION_STRING="${PROJECT_VERSION}")
osp_apply_settings(osp_engine)

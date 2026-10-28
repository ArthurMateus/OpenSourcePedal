# Every .cpp under src/ is part of the library; adding a file adds no line here (#115).
file(GLOB_RECURSE OSP_ENGINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(osp_engine STATIC ${OSP_ENGINE_SOURCES})
add_library(osp::engine ALIAS osp_engine)

target_include_directories(osp_engine PUBLIC include)
target_link_libraries(osp_engine PUBLIC osp_dsp_core PRIVATE nlohmann_json::nlohmann_json)
# Header-only FFT for the tone-match analysis, used only inside src/ (ADR-0008).
target_link_libraries(osp_engine PRIVATE signalsmith-dsp)
target_compile_definitions(osp_engine PRIVATE OSP_VERSION_STRING="${PROJECT_VERSION}")
osp_apply_settings(osp_engine)

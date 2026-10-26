# tools/: JUCE-free offline tools (ADR-0006 D3). Third-party code is built without our warning flags.
add_library(osp_ebur128 STATIC "${libebur128_SOURCE_DIR}/ebur128/ebur128.c")
target_include_directories(osp_ebur128 SYSTEM PUBLIC "${libebur128_SOURCE_DIR}/ebur128")
# MSVC has no <sys/queue.h>; libebur128 ships its own copy for that case.
target_include_directories(osp_ebur128 PRIVATE "${libebur128_SOURCE_DIR}/ebur128/queue")
target_compile_definitions(osp_ebur128 PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)

# osp_dr_wav is defined in cmake/OspDependencies.cmake (shared with the tests).
add_executable(osp-render render/osp_render.cpp)
target_link_libraries(osp-render PRIVATE osp_engine osp_ebur128 osp_dr_wav)
osp_apply_settings(osp-render)

# Prints the HIIR halfband coefficients committed in dsp-core/src/oversampling/Oversampler.cpp (ADR-0005 D1, D3).
add_executable(osp-hiir-design design/hiir_coefs.cpp "${hiir_SOURCE_DIR}/hiir/PolyphaseIir2Designer.cpp")
target_include_directories(osp-hiir-design SYSTEM PRIVATE "${hiir_SOURCE_DIR}")

# Tone match (ADR-0008): reference WAV/MP3 + the player's DI -> preset JSON. dr_mp3 comes from the same dr_libs commit.
add_library(osp_dr_mp3 STATIC tonematch/dr_mp3_impl.c)
target_include_directories(osp_dr_mp3 SYSTEM PUBLIC "${dr_libs_SOURCE_DIR}")
target_compile_definitions(osp_dr_mp3 PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(osp-tonematch tonematch/osp_tonematch.cpp)
# activeCodePage UTF-8 (Windows 10 1903+): argv and fopen use UTF-8, so non-ASCII names and paths work (#120 review).
if(MSVC)
    target_sources(osp-tonematch PRIVATE tonematch/utf8.manifest)
endif()
target_link_libraries(osp-tonematch PRIVATE osp_engine osp_dr_wav osp_dr_mp3)
target_compile_definitions(osp-tonematch PRIVATE OSP_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets" _CRT_SECURE_NO_WARNINGS)
osp_apply_settings(osp-tonematch)

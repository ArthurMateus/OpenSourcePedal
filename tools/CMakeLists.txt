# tools/: JUCE-free offline tools (ADR-0006 D3). Third-party code is built without our warning flags.
add_library(osp_ebur128 STATIC "${libebur128_SOURCE_DIR}/ebur128/ebur128.c")
target_include_directories(osp_ebur128 SYSTEM PUBLIC "${libebur128_SOURCE_DIR}/ebur128")
# MSVC has no <sys/queue.h>; libebur128 ships its own copy for that case.
target_include_directories(osp_ebur128 PRIVATE "${libebur128_SOURCE_DIR}/ebur128/queue")
target_compile_definitions(osp_ebur128 PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)

add_library(osp_dr_wav STATIC render/dr_wav_impl.c)
target_include_directories(osp_dr_wav SYSTEM PUBLIC "${dr_libs_SOURCE_DIR}")
target_compile_definitions(osp_dr_wav PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(osp-render render/osp_render.cpp)
target_link_libraries(osp-render PRIVATE osp_engine osp_ebur128 osp_dr_wav)
osp_apply_settings(osp-render)

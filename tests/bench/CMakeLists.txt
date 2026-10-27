# osp-bench (ADR-0006 D3, ADR-0007 §5 / REQ-TEST-007). Third-party code is built without our warning flags.
add_library(osp_nanobench STATIC nanobench_impl.cpp)
target_include_directories(osp_nanobench SYSTEM PUBLIC "${nanobench_SOURCE_DIR}/src/include")
target_compile_features(osp_nanobench PUBLIC cxx_std_20)

# Each benchmark is one self-registering file in entries/ (bench_common.h), so adding a block touches no shared list.
file(GLOB OSP_BENCH_ENTRIES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/entries/*.cpp")
add_executable(osp-bench osp_bench.cpp ${OSP_BENCH_ENTRIES})
target_include_directories(osp-bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(osp-bench PRIVATE osp_engine osp_nanobench)
osp_apply_settings(osp-bench)
# AGENTS.md section 3 runs it as build/<preset>/tests/osp-bench.
set_target_properties(osp-bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
target_compile_definitions(osp-bench PRIVATE _CRT_SECURE_NO_WARNINGS) # std::fopen for the JSON output

add_executable(
    flowforge_script_runtime_integration_test
    flowforge_script_runtime_integration_test.cpp
)
target_link_libraries(
    flowforge_script_runtime_integration_test
    PRIVATE
        lux::engine::flowforge::flowforge_compiler
        lux::engine::simulation::simulation_composition
        lux::engine::simulation::simulation_script
        lux::engine::simulation::simulation_script_native
        lux::engine::core::task
)
if(MSVC)
    target_compile_options(flowforge_script_runtime_integration_test PRIVATE /UNDEBUG)
else()
    target_compile_options(flowforge_script_runtime_integration_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  flowforge_script_runtime_integration_test
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME flowforge_script_runtime_integration_test
    COMMAND flowforge_script_runtime_integration_test
)
set_tests_properties(
    flowforge_script_runtime_integration_test
    PROPERTIES ENVIRONMENT "LUX_FLOWFORGE_LINKER=${CMAKE_LINKER}"
)

add_executable(
    flowforge_script_runtime_benchmark
    flowforge_script_runtime_integration_test.cpp
)
target_link_libraries(
    flowforge_script_runtime_benchmark
    PRIVATE
        lux::engine::flowforge::flowforge_compiler
        lux::engine::simulation::simulation_composition
        lux::engine::simulation::simulation_script
        lux::engine::simulation::simulation_script_native
        lux::engine::core::task
)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE LUX_FLOWFORGE_BENCHMARK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(LUX_FLOWFORGE_BENCHMARK_GIT_COMMIT STREQUAL "")
    set(LUX_FLOWFORGE_BENCHMARK_GIT_COMMIT "unknown")
endif()
target_compile_definitions(
    flowforge_script_runtime_benchmark
    PRIVATE
        LUX_BENCHMARK_GIT_COMMIT="${LUX_FLOWFORGE_BENCHMARK_GIT_COMMIT}"
        LUX_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
if(MSVC)
    target_compile_options(flowforge_script_runtime_benchmark PRIVATE /UNDEBUG)
else()
    target_compile_options(flowforge_script_runtime_benchmark PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  flowforge_script_runtime_benchmark
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME flowforge_script_runtime_benchmark_smoke
    COMMAND flowforge_script_runtime_benchmark
        --group micro-flowforge-sync
        --size 8
        --frames 2
        --warmups 1
        --resume-budget 8
        --output ${CMAKE_CURRENT_BINARY_DIR}/flowforge-benchmark-smoke.csv
)
set_tests_properties(
    flowforge_script_runtime_benchmark_smoke
    PROPERTIES ENVIRONMENT "LUX_FLOWFORGE_LINKER=${CMAKE_LINKER}"
)

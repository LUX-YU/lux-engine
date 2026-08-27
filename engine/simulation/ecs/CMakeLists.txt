set(_lux_ecs_components)
foreach(component IN ITEMS
    core
    schema
    snapshot
    ecs_task
    hierarchy
    transform
)
    if(TARGET ${component})
        list(APPEND _lux_ecs_components ${component})
    endif()
endforeach()

if(_lux_ecs_components)
    install_components(
        PROJECT_NAME lux-engine-simulation-ecs
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::simulation::ecs
        COMPONENTS   ${_lux_ecs_components}
    )
endif()

add_executable(ecs_l1_benchmark test/l1_benchmark.cpp)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE LUX_SIMULATION_ECS_BENCHMARK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(LUX_SIMULATION_ECS_BENCHMARK_GIT_COMMIT STREQUAL "")
    set(LUX_SIMULATION_ECS_BENCHMARK_GIT_COMMIT "unknown")
endif()
target_include_directories(
    ecs_l1_benchmark
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/hierarchy/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/hierarchy/sinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/transform/pinclude
)
target_compile_definitions(
    ecs_l1_benchmark
    PRIVATE
        LUX_SIMULATION_ECS_SNAPSHOT_TESTING=1
        LUX_BENCHMARK_GIT_COMMIT="${LUX_SIMULATION_ECS_BENCHMARK_GIT_COMMIT}"
        LUX_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
if(MSVC)
    target_compile_options(ecs_l1_benchmark PRIVATE /bigobj)
endif()
target_link_libraries(
    ecs_l1_benchmark
    PRIVATE
        lux::engine::simulation::system
        lux::engine::simulation::ecs::task
        lux::engine::simulation::ecs::snapshot
        lux::engine::simulation::ecs::hierarchy
        lux::engine::simulation::ecs::transform
        lux::engine::simulation::simulation_script_binding
        lux::engine::core::object
)
lux_classify_target(
    TARGET  ecs_l1_benchmark
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    FOUNDATION
)

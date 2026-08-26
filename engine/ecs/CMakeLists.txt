foreach(component IN ITEMS
    core
    system
    schema
    snapshot
    hierarchy
    transform
)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${component}/CMakeLists.txt")
        add_subdirectory(${component})
    endif()
endforeach()

set(_lux_ecs_components)
foreach(component IN ITEMS
    core
    system
    schema
    snapshot
    hierarchy
    transform
)
    if(TARGET ${component})
        list(APPEND _lux_ecs_components ${component})
    endif()
endforeach()

if(_lux_ecs_components)
    install_components(
        PROJECT_NAME lux-engine-ecs
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::ecs
        COMPONENTS   ${_lux_ecs_components}
    )
endif()

add_executable(ecs_l1_benchmark test/l1_benchmark.cpp)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE LUX_ECS_BENCHMARK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(LUX_ECS_BENCHMARK_GIT_COMMIT STREQUAL "")
    set(LUX_ECS_BENCHMARK_GIT_COMMIT "unknown")
endif()
target_include_directories(
    ecs_l1_benchmark
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/hierarchy/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/system/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/transform/pinclude
)
target_compile_definitions(
    ecs_l1_benchmark
    PRIVATE
        LUX_ECS_SNAPSHOT_TESTING=1
        LUX_BENCHMARK_GIT_COMMIT="${LUX_ECS_BENCHMARK_GIT_COMMIT}"
        LUX_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
if(MSVC)
    target_compile_options(ecs_l1_benchmark PRIVATE /bigobj)
endif()
target_link_libraries(
    ecs_l1_benchmark
    PRIVATE
        lux::engine::ecs::system
        lux::engine::ecs::snapshot
        lux::engine::ecs::hierarchy
        lux::engine::ecs::transform
        lux::engine::core::object
)
lux_classify_target(
    TARGET  ecs_l1_benchmark
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)

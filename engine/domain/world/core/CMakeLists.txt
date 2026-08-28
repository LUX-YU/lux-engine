find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_WORLD_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_WORLD_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_WORLD_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/world/visibility.h
)

add_component(
    COMPONENT_NAME world
    NAMESPACE      lux::engine
    OUTPUT_NAME    lux_engine_world
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldIdentity.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldDescription.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldDescriptionBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldFailureInjection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartition.cpp
)

component_include_directories(
    world
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_include_directories(
    world
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(world PRIVATE LUX_ENGINE_WORLD_LIBRARY)
target_link_libraries(world PUBLIC stduuid lux::cxx::compile_time)
component_add_transitive_commands(
    world
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  world
    LAYER   WORLD
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(world_contract_test test/world_contract_test.cpp)
target_link_libraries(world_contract_test PRIVATE lux::engine::world)
target_include_directories(
    world_contract_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
if(MSVC)
    target_compile_options(world_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(world_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  world_contract_test
    LAYER   WORLD
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME world_contract_test COMMAND world_contract_test)

add_executable(world_diagnostic_benchmark test/world_diagnostic_benchmark.cpp)
target_link_libraries(world_diagnostic_benchmark PRIVATE lux::engine::world)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE LUX_WORLD_BENCHMARK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(LUX_WORLD_BENCHMARK_GIT_COMMIT STREQUAL "")
    set(LUX_WORLD_BENCHMARK_GIT_COMMIT "unknown")
endif()
target_compile_definitions(
    world_diagnostic_benchmark
    PRIVATE
        LUX_BENCHMARK_GIT_COMMIT="${LUX_WORLD_BENCHMARK_GIT_COMMIT}"
        LUX_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
lux_classify_target(
    TARGET  world_diagnostic_benchmark
    LAYER   WORLD
    PRODUCT TEST
    ROLE    FOUNDATION
)

install_components(
    PROJECT_NAME lux-engine-world
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine
    COMPONENTS   world
)

find_package(lux-cxx REQUIRED COMPONENTS memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PROCESS_WORLD_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PROCESS_WORLD_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PROCESS_WORLD_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/process/world/visibility.h
)

add_component(
    COMPONENT_NAME process_world
    NAMESPACE      lux::engine::process
    OUTPUT_NAME    lux_engine_process_world
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartitionLoad.cpp
)
component_include_directories(
    process_world
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(process_world PRIVATE LUX_ENGINE_PROCESS_WORLD_LIBRARY)
target_link_libraries(
    process_world
    PUBLIC
        lux::engine::process::process_execution
        lux::engine::world::storage
    PRIVATE lux::cxx::memory
)
component_add_transitive_commands(
    process_world
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
)
lux_classify_target(
    TARGET  process_world
    LAYER   PROCESS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(
    process_world_test
    test/process_world_test.cpp
)
target_link_libraries(
    process_world_test
    PRIVATE lux::engine::process::process_world
)
if(MSVC)
    target_compile_options(process_world_test PRIVATE /UNDEBUG)
else()
    target_compile_options(process_world_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  process_world_test
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME process_world_test COMMAND process_world_test)

install_components(
    PROJECT_NAME lux-engine-process-world
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::process
    COMPONENTS   process_world
)

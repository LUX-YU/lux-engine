find_package(lux-cxx REQUIRED COMPONENTS memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PROCESS_WORLD_LOADING_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PROCESS_WORLD_LOADING_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PROCESS_WORLD_LOADING_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/process/world_loading/visibility.h
)

add_component(
    COMPONENT_NAME process_world_loading
    NAMESPACE      lux::engine::process
    OUTPUT_NAME    lux_engine_process_world_loading
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartitionLoad.cpp
)
add_library(lux::engine::process::world_loading ALIAS process_world_loading)
component_include_directories(
    process_world_loading
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(process_world_loading PRIVATE LUX_ENGINE_PROCESS_WORLD_LOADING_LIBRARY)
target_link_libraries(
    process_world_loading
    PUBLIC
        lux::engine::process::process_execution
        lux::engine::world::storage
    PRIVATE lux::cxx::memory
)
component_add_transitive_commands(
    process_world_loading
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
    "add_library(lux::engine::process::world_loading INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::process::world_loading PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::process::process_world_loading)"
)
lux_classify_target(
    TARGET  process_world_loading
    LAYER   PROCESS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(
    process_world_loading_test
    test/process_world_loading_test.cpp
)
target_link_libraries(
    process_world_loading_test
    PRIVATE lux::engine::process::world_loading
)
if(MSVC)
    target_compile_options(process_world_loading_test PRIVATE /UNDEBUG)
else()
    target_compile_options(process_world_loading_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  process_world_loading_test
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME process_world_loading_test COMMAND process_world_loading_test)

install_components(
    PROJECT_NAME lux-engine-process-world-loading
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::process
    COMPONENTS   process_world_loading
)

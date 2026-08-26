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
)

component_include_directories(
    world
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
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

install_components(
    PROJECT_NAME lux-engine-world
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine
    COMPONENTS   world
)

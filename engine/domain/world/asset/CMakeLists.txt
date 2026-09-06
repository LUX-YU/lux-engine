find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_WORLD_ASSET_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_WORLD_ASSET_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_WORLD_ASSET_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/world/asset/visibility.h
)

add_component(
    COMPONENT_NAME world_asset
    NAMESPACE      lux::engine::world
    OUTPUT_NAME    lux_engine_world_asset
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldAssetCodec.cpp
)
add_library(lux::engine::world::asset ALIAS world_asset)
component_include_directories(
    world_asset
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    world_asset
    PRIVATE LUX_ENGINE_WORLD_ASSET_LIBRARY
)
target_link_libraries(
    world_asset
    PUBLIC
        lux::engine::world::description
        lux::engine::resource::asset
    PRIVATE lux::engine::core::serialization
)
component_add_transitive_commands(
    world_asset
    "find_package(lux-engine-world-description REQUIRED COMPONENTS world_description)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "if(NOT TARGET lux::engine::world::asset)"
    "add_library(lux::engine::world::asset INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::world::asset PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::world::world_asset)"
    "endif()"
)
lux_classify_target(
    TARGET  world_asset
    LAYER   WORLD
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

install_components(
    PROJECT_NAME lux-engine-world-asset
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::world
    COMPONENTS   world_asset
)

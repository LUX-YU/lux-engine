find_package(lux-cxx REQUIRED COMPONENTS algorithm memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_WORLD_STORAGE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_WORLD_STORAGE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_WORLD_STORAGE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/world/storage/visibility.h
)

add_component(
    COMPONENT_NAME world_storage
    NAMESPACE      lux::engine::world
    OUTPUT_NAME    lux_engine_world_storage
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneStreamingQuery.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneStreamingCodec.cpp
)

component_include_directories(
    world_storage
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME include
)
target_compile_definitions(world_storage PRIVATE LUX_ENGINE_WORLD_STORAGE_LIBRARY)
target_link_libraries(
    world_storage
    PUBLIC
        lux::engine::world::description
        lux::engine::scene::scene_description
        lux::cxx::algorithm lux::cxx::memory
    PRIVATE
        lux::engine::core::serialization
)
component_add_transitive_commands(
    world_storage
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm memory)"
    "find_package(lux-engine-world-description REQUIRED COMPONENTS world_description)"
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-world-storage
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::world
    COMPONENTS   world_storage
)

find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_AUTHORING_WORLD_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_AUTHORING_WORLD_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/authoring/world/visibility.h
)

add_component(
    COMPONENT_NAME authoring_world
    NAMESPACE      lux::engine::authoring
    OUTPUT_NAME    lux_engine_authoring_world
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldIdentifiers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldPartition.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSourceCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldDescriptorIndex.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldAuthoringTransaction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldTerrainAuthoring.cpp
)

lux_classify_target(
    TARGET  authoring_world
    LAYER   AUTHORING
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

component_include_directories(
    authoring_world
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    authoring_world
    PRIVATE LUX_ENGINE_AUTHORING_WORLD_LIBRARY
)

target_link_libraries(
    authoring_world
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::asset
        lux::engine::core::math
        lux::cxx::algorithm
        lux::engine::core::serialization
        lux::engine::extensions::extension_identity
        stduuid
)

component_add_transitive_commands(
    authoring_world
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)"
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math serialization)"
    "find_package(lux-engine-extensions REQUIRED COMPONENTS extension_identity)"
)

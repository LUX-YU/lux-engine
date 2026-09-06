## Engine-owned Scene data and synchronous asset SerDeser. Runtime World,
## Schedule, IO, async execution and product lifecycle remain in their owners.

find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time core)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/visibility.h
)

add_component(
    COMPONENT_NAME scene
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneAssetSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneDescriptionValidation.cpp
)

component_include_directories(
    scene
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(scene PRIVATE LUX_ENGINE_SCENE_LIBRARY)

target_link_libraries(
    scene
    PUBLIC
        lux::cxx::algorithm
        lux::cxx::compile_time
        lux::cxx::core
        lux::engine::core::serialization
        lux::engine::resource::asset
        lux::engine::ecs::identity
        lux::engine::ecs::scene_format
        lux::engine::extensions::extension_api
)

component_add_transitive_commands(
    scene
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS identity scene_format)"
    "find_package(lux-engine-extensions REQUIRED COMPONENTS extension_api)"
)

lux_classify_target(
    TARGET  scene
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

install_components(
    PROJECT_NAME    lux-engine-scene
    VERSION         0.0.1
    NAMESPACE       lux::engine::scene
    COMPONENTS      scene
)

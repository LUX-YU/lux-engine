generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/entity_scene/visibility.h
)

add_component(
    COMPONENT_NAME toolchain_entity_scene_cook
    STATIC
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_entity_scene_cook
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TaggedPayloadTranscoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySectionImageBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntitySceneCooker.cpp
)

lux_classify_target(
    TARGET  toolchain_entity_scene_cook
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

component_include_directories(
    toolchain_entity_scene_cook
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    toolchain_entity_scene_cook
    PUBLIC LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_DLL_DISABLE
)

target_link_libraries(
    toolchain_entity_scene_cook
    PUBLIC
        lux::engine::ecs::scene_format
        lux::engine::scene::scene
    PRIVATE
        lux::engine::core::serialization
        lux::cxx::algorithm
)

component_add_transitive_commands(
    toolchain_entity_scene_cook
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS scene_format)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)

add_executable(
    entity_scene_cooker_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/entity_scene_cooker_test.cpp
)
target_link_libraries(
    entity_scene_cooker_test
    PRIVATE
        lux::engine::toolchain::toolchain_entity_scene_cook
        lux::engine::core::serialization
)
if(MSVC)
    target_compile_options(entity_scene_cooker_test PRIVATE /UNDEBUG)
else()
    target_compile_options(entity_scene_cooker_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  entity_scene_cooker_test
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)

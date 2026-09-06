find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_DESCRIPTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_DESCRIPTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_DESCRIPTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/description/visibility.h
)

add_component(
    COMPONENT_NAME scene_description
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_description
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneDescription.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneDescriptionBuilder.cpp
)
component_include_directories(
    scene_description
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(scene_description PRIVATE LUX_ENGINE_SCENE_DESCRIPTION_LIBRARY)
target_link_libraries(
    scene_description
    PUBLIC
        lux::engine::resource::identity
        lux::engine::system::domain_system_identity
        lux::engine::system::domain_system_description
        lux::cxx::compile_time
)
component_add_transitive_commands(
    scene_description
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
    "find_package(lux-engine-domain-system-identity REQUIRED COMPONENTS domain_system_identity)"
    "find_package(lux-engine-domain-system-description REQUIRED COMPONENTS domain_system_description)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)
lux_classify_target(TARGET scene_description LAYER SCENE PRODUCT RUNTIME ROLE FOUNDATION)

install_components(
    PROJECT_NAME lux-engine-scene-description
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_description
)

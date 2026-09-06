generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_CONTENT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_CONTENT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_CONTENT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/content/visibility.h
)

add_component(
    COMPONENT_NAME content
    NAMESPACE      lux::engine
    OUTPUT_NAME    lux_engine_content
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/BuiltinAssetIds.cpp
)

component_include_directories(
    content
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(content PRIVATE LUX_ENGINE_CONTENT_LIBRARY)
target_link_libraries(content PUBLIC lux::engine::resource::asset)
component_add_transitive_commands(
    content
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)
lux_classify_target(
    TARGET  content
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

install_components(
    PROJECT_NAME lux-engine-content
    VERSION      0.0.1
    NAMESPACE    lux::engine
    COMPONENTS   content
)

find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME
        LUX_ENGINE_FUNCTION_RENDER_STANDARD_CONTENT_LIBRARY
    PUBLIC_MACRO_NAME
        LUX_ENGINE_FUNCTION_RENDER_STANDARD_CONTENT_PUBLIC
    DISABLE_DLL_MACRO_NAME
        LUX_ENGINE_FUNCTION_RENDER_STANDARD_CONTENT_DLL_DISABLE
    GENERATE_FILE_PATH
        lux/engine/function/render/standard/content/visibility.h
)

add_component(
    COMPONENT_NAME render_standard_content
    NAMESPACE      lux::engine::function
    OUTPUT_NAME    lux_engine_function_render_standard_content
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ClassicMeshBatchCodec.cpp
)

component_include_directories(
    render_standard_content
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    render_standard_content
    PRIVATE LUX_ENGINE_FUNCTION_RENDER_STANDARD_CONTENT_LIBRARY
)

target_link_libraries(
    render_standard_content
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::identity
    PRIVATE
        lux::engine::core::serialization
)

component_add_transitive_commands(
    render_standard_content
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
)

lux_classify_target(
    TARGET  render_standard_content
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

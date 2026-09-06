find_package(spirv_cross_core CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_SHADER_ASSET_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_SHADER_ASSET_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_SHADER_ASSET_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/asset/shader/visibility.h
)

add_component(
    STATIC
    COMPONENT_NAME toolchain_shader_asset_cooker
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_shader_asset_cooker
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderCooker.cpp
        $<TARGET_OBJECTS:toolchain_shader_reflection>
)

component_include_directories(
    toolchain_shader_asset_cooker
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../../shader/sinclude
)

target_compile_definitions(
    toolchain_shader_asset_cooker
    PUBLIC LUX_ENGINE_TOOLCHAIN_SHADER_ASSET_DLL_DISABLE
    PRIVATE LUX_ENGINE_TOOLCHAIN_SHADER_ASSET_LIBRARY
)

target_link_libraries(
    toolchain_shader_asset_cooker
    PUBLIC lux::engine::resource::asset
    PRIVATE spirv-cross-core
)

component_add_transitive_commands(
    toolchain_shader_asset_cooker
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset description)"
)

lux_classify_target(
    TARGET  toolchain_shader_asset_cooker
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

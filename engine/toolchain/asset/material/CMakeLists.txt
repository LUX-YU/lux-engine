set(LUX_MATERIAL_SHADER_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/modules/function/render/features/assets/shaders")
set(LUX_MATERIAL_SHADER_EMITTED_DIR "${CMAKE_BINARY_DIR}/lglsl_emitted")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/pinclude/lux/engine/toolchain/asset/material/MaterialShaderPaths.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/pinclude/lux/engine/toolchain/asset/material/MaterialShaderPaths.hpp
    @ONLY
)

if(NOT TARGET lux::engine::toolchain::toolchain_shader_compiler)
    message(FATAL_ERROR
        "toolchain_material_compiler requires the configured "
        "lux::engine::toolchain::toolchain_shader_compiler target"
    )
endif()

add_component(
    STATIC
    COMPONENT_NAME toolchain_material_compiler
    NAMESPACE      lux::engine::toolchain
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialGraphCompiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialLowering.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialToGraph.cpp
)

lux_classify_target(
    TARGET  toolchain_material_compiler
    LAYER   TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

component_include_directories(
    toolchain_material_compiler
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
    PRIVATE           ${CMAKE_CURRENT_BINARY_DIR}/pinclude
)

target_link_libraries(
    toolchain_material_compiler
    PUBLIC
        lux::engine::resource::asset
        lux::engine::authoring::authoring_assets
        lux::engine::toolchain::toolchain_shader_ir
    PRIVATE
        lux::engine::toolchain::toolchain_shader_compiler
)

component_add_transitive_commands(
    toolchain_material_compiler
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_assets)"
    "find_package(lux-engine-toolchain REQUIRED COMPONENTS toolchain_shader_ir toolchain_shader_compiler)"
)

## engine::toolchain_shader_compiler
##
## GLSL backend for ShaderGen (the engine half): lowers backend-neutral ShaderIR
## into GLSL source text, then compiles it to SPIR-V via libshaderc. Follows the
## same pattern as the old material_graph_glsl, but:
##   - it consumes the generic ShaderIR (not MatIR), so it can serve materials,
##     GBuffer layout, and lighting as three separate clients going forward;
##   - it wires up an IncluderInterface so shaders #include the real
##     single-source-of-truth files (gbuffer_encode.glsl, etc.) instead of
##     hand-copied inline text; the include search path is supplied by the
##     caller, so this component has no dependency on the render module.
##
## Editor tier (engine/) -- never linked into the shipped runtime; the runtime only consumes SPIR-V.
## Build target: lux::engine::toolchain::toolchain_shader_compiler

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_TOOLCHAIN_SHADER_COMPILER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_TOOLCHAIN_SHADER_COMPILER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_TOOLCHAIN_SHADER_COMPILER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/shader/visibility.h
)

option(LUX_ENABLE_SHADERGEN
       "Build the shadergen GLSL->SPIR-V backend (editor/cook-side)" OFF)

if(NOT LUX_ENABLE_SHADERGEN)
    return()
endif()

# shaderc::shaderc is the toolchain-local bridge. Runtime/resource targets do
# not discover or propagate the compiler library.
if(NOT TARGET shaderc::shaderc)
    message(FATAL_ERROR
        "toolchain_shader_compiler requires the shaderc::shaderc bridge target "
        "(defined by engine/toolchain/shader). Configure toolchain first.")
endif()

set(SHADERGEN_GLSL_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderEmitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderCompiler.cpp
)

add_component(
    COMPONENT_NAME  toolchain_shader_compiler
    NAMESPACE       lux::engine::toolchain
    OUTPUT_NAME     lux_engine_toolchain_shader_compiler
    SOURCE_FILES    ${SHADERGEN_GLSL_SRCS}
)

lux_classify_target(
    TARGET toolchain_shader_compiler
    LAYER TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE DOMAIN
)

component_include_directories(
    toolchain_shader_compiler
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    toolchain_shader_compiler
    PRIVATE
    LUX_TOOLCHAIN_SHADER_COMPILER_LIBRARY
)

target_link_libraries(
    toolchain_shader_compiler
    PUBLIC
    lux::engine::toolchain::toolchain_shader_ir
    PRIVATE
    shaderc::shaderc
    lux::engine::toolchain::toolchain_shader
)

component_add_transitive_commands(
    toolchain_shader_compiler
    "find_package(lux-engine-toolchain REQUIRED COMPONENTS toolchain_shader_ir)"
)

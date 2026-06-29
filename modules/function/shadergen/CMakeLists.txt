# =============================================================================
#  function::shadergen  —  着色器合成子系统【中性核】（IR + lowering 框架）
# -----------------------------------------------------------------------------
#  可复用层，**不得引入任何重型编译依赖**（shaderc/SPIR-V/MLIR）。本模块只发出
#  后端中性的 ShaderIR (ShaderIR.hpp)；GLSL codegen 后端在 editor 层的
#  lux::shadergen::glsl (engine/shadergen_glsl/)，消费 ShaderIR。
#  （镜像 material_graph(function) / material_graph_glsl(engine) 的拆分范式。）
#
#  唯一链接依赖：lux::engine::resource::description（材质/着色契约 SSOT）。
#  设计：.internal/plan/shadergen-design.md
# =============================================================================

set(SHADERGEN_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderIR.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/material/MaterialLowering.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/material/MaterialToGraph.cpp
)

add_component(
    COMPONENT_NAME shadergen
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${SHADERGEN_SRCS}
)

target_compile_definitions(
    shadergen
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

component_include_directories(
    shadergen
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_link_libraries(
    shadergen
    PUBLIC
    lux::engine::resource::description
)

component_add_transitive_commands(
    shadergen
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)

set(ENABLE_SHADERGEN_TEST OFF CACHE BOOL "Enable shadergen tests")
if(ENABLE_SHADERGEN_TEST)
    add_subdirectory(test)
endif()

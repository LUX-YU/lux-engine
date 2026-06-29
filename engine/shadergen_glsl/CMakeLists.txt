## engine::shadergen_glsl
##
## ShaderGen 的 GLSL 后端（engine 半）：把后端中性的 ShaderIR lower 成 GLSL 源文本，
## 用 libshaderc 编译为 SPIR-V。与旧 material_graph_glsl 同范式，但：
##   - 吃通用 ShaderIR（非 MatIR），将来同时服务材质/GBuffer 布局/光照三个客户；
##   - 配 IncluderInterface，外壳 #include 真 SSOT（gbuffer_encode.glsl 等），不再
##     内联手抄；include 搜索路径由【调用方传入】，故本组件不依赖 render 模块。
##
## Editor tier (engine/) — 永不链接进 shipped runtime；runtime 只消费 SPIR-V。
## Build target: lux::engine::shadergen_glsl

option(LUX_ENABLE_SHADERGEN
       "Build the shadergen GLSL->SPIR-V backend (editor/cook-side)" OFF)

if(NOT LUX_ENABLE_SHADERGEN)
    return()
endif()

# shaderc::shaderc 是 modules/resource/asset 建的 INTERFACE 桥（resource 层，先于
# engine/ 配置）。缺失则明确报错，而非晦涩的链接错误。
if(NOT TARGET shaderc::shaderc)
    message(FATAL_ERROR
        "shadergen_glsl requires the shaderc::shaderc bridge target "
        "(defined by modules/resource/asset). Configure the resource layer first.")
endif()

set(SHADERGEN_GLSL_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderEmitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderCompiler.cpp
)

add_component(
    COMPONENT_NAME  shadergen_glsl
    NAMESPACE       lux::engine
    OUTPUT_NAME     lux_engine_shadergen_glsl
    SOURCE_FILES    ${SHADERGEN_GLSL_SRCS}
)

component_include_directories(
    shadergen_glsl
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    shadergen_glsl
    PRIVATE
    LUX_EDITOR_LIBRARY
)

target_link_libraries(
    shadergen_glsl
    PUBLIC
    lux::engine::function::shadergen
    PRIVATE
    shaderc::shaderc
)

component_add_transitive_commands(
    shadergen_glsl
    "find_package(lux-engine-function REQUIRED COMPONENTS shadergen)"
)

set(ENABLE_SHADERGEN_GLSL_TEST OFF CACHE BOOL "Enable shadergen_glsl tests")
if(ENABLE_SHADERGEN_GLSL_TEST)
    add_subdirectory(test)
endif()

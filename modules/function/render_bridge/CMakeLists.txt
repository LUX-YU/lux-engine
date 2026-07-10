# lux::engine::function::render_bridge — the ECS→renderer bridge kernel
# (ADR: lux-engine-pack-architecture)
#
# RenderableSystem + IRenderableBridge + the 3 generic bridges + EcsRenderTraits
# primary template + RenderableBridgeContext. Pure mechanism: it knows the ECS
# kernel and the render CLIENT SDK, never a domain component (those live in
# packs/, which extend this kernel via open trait specialisation / addBridge).
# namespace: lux::render_bridge

set(LUX_RENDER_BRIDGE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderableSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderableBridgeContext.cpp
)

add_component(
    COMPONENT_NAME  render_bridge
    NAMESPACE       lux::engine::function
    OUTPUT_NAME     lux_engine_function_render_bridge
    SOURCE_FILES    ${LUX_RENDER_BRIDGE_SRCS}
)

component_include_directories(
    render_bridge
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    render_bridge
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    render_bridge
    PUBLIC
    lux::engine::function::ecs
    lux::engine::function::render      # RenderSession / proxies / feature ops (client SDK)
    lux::engine::resource::asset       # RenderableBridgeContext resolves assets
    lux::engine::core::math
    lux::engine::core::meta
)

component_add_transitive_commands(
    render_bridge
    "find_package (lux-engine-function REQUIRED COMPONENTS ecs render)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package (lux-engine-core     REQUIRED COMPONENTS math meta)"
)

add_component(
    STATIC
    COMPONENT_NAME render_runtime
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_runtime
    SOURCE_FILES src/DeviceSession.cpp src/Backend.cpp src/Runtime.cpp src/Presentation.cpp
)

component_include_directories(
    render_runtime
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
    INSTALL_TIME include
)

target_link_libraries(render_runtime
    PUBLIC lux::engine::render::render_core lux::engine::render::render_transport render_graph
    PRIVATE render_vulkan_shader_compiler
)

target_compile_features(render_runtime PUBLIC cxx_std_20)

target_compile_definitions(render_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# The existing luxop parser/template produce the actual Runtime protocol traits.
include("${CMAKE_CURRENT_SOURCE_DIR}/../transport/cmake/RenderTransportCodegen.cmake")

target_include_directories(render_runtime PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../core/type_info/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
)

lux_add_codegen_job(
    NAME render_runtime_operations
    MARKER luxop
    TARGET_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/render/runtime/RuntimeMessages.hpp"
    LOGICAL_PATHS "lux/engine/render/runtime/RuntimeMessages.hpp"
    EXTRA_COMPILE_OPTIONS -D__LUX_PARSE_TIME__=1
)

lux_codegen_add_projection(
    JOB render_runtime_operations
    NAME transport_traits
    TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/../transport/cmake/transport_ops.template"
    OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated"
    OUTPUT_SUFFIX .ops.hpp
)

lux_target_add_codegen(TARGET render_runtime JOB render_runtime_operations)

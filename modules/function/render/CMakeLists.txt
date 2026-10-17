add_subdirectory(core)

add_subdirectory(data)

add_subdirectory(protocol)

if(LUX_RENDER_CLIENT_ONLY)
    add_subdirectory(runtime)

    lux_engine_install_components(
        PROJECT_NAME lux-engine-render
        VERSION 0.0.1
        NAMESPACE lux::engine::render
        COMPONENTS render_core render_data render_protocol render_client
    )
    return()
endif()

add_subdirectory(resource)

add_subdirectory(graph)

add_subdirectory(features)

set(LUX_RENDER_BACKEND "VULKAN" CACHE STRING "Selected Render Native implementation")
set_property(CACHE LUX_RENDER_BACKEND PROPERTY STRINGS VULKAN)

if(NOT LUX_RENDER_BACKEND STREQUAL "VULKAN")
    message(FATAL_ERROR "Unsupported LUX_RENDER_BACKEND: ${LUX_RENDER_BACKEND}")
endif()

add_subdirectory(native/vulkan)

add_subdirectory(runtime)

if(BUILD_TESTING)
    add_subdirectory(examples/basic_mesh)
endif()

lux_engine_install_components(
    PROJECT_NAME lux-engine-render
    VERSION 0.0.1
    NAMESPACE lux::engine::render
    COMPONENTS render_core render_data render_protocol render_protocol_async render_resource render_graph render_features render_native_vulkan
        render_native_vulkan_extensions render_native_vulkan_features render_client render_runtime
)

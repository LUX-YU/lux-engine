add_component(
    STATIC
    COMPONENT_NAME render_client
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_client
    SOURCE_FILES src/RenderClient.cpp src/Wire.cpp
)

component_include_directories(render_client
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE pinclude
    INSTALL_TIME include
)

component_add_internal_dependencies(render_client
    lux::engine::render::render_core lux::engine::render::render_data lux::engine::render::render_protocol
)

target_link_libraries(render_client PUBLIC lux::engine::platform::platform_ipc)

component_add_transitive_commands(render_client
    [=[find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS platform_ipc
        HINTS "${CMAKE_CURRENT_LIST_DIR}/../../lux-engine-platform")]=]
)

add_component(
    STATIC
    COMPONENT_NAME render_runtime
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_runtime
    SOURCE_FILES src/RenderServer.cpp
)

component_include_directories(render_runtime
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE pinclude
    INSTALL_TIME include
)

component_add_internal_dependencies(render_runtime
    lux::engine::render::render_client lux::engine::render::render_features
    lux::engine::render::render_native_vulkan
)

target_link_libraries(render_runtime PRIVATE lux::engine::platform::platform_ipc bcrypt)

foreach(component render_client render_runtime)
    target_compile_features(${component} PUBLIC cxx_std_20)
endforeach()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

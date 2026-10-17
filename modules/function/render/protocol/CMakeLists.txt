find_package(lux-cxx CONFIG REQUIRED COMPONENTS core)

add_component(
    STATIC
    COMPONENT_NAME render_protocol
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_protocol
    SOURCE_FILES src/Protocol.cpp
)

component_include_directories(
    render_protocol
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_protocol lux::engine::render::render_core)

target_link_libraries(render_protocol PUBLIC lux::cxx::core)

component_add_transitive_commands(
    render_protocol
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS core)"
)

target_compile_features(render_protocol PUBLIC cxx_std_20)

if(NOT LUX_RENDER_CLIENT_ONLY)
    find_package(stdexec CONFIG REQUIRED)

    add_interface_component(
        COMPONENT_NAME render_protocol_async
        NAMESPACE lux::engine::render
    )

    component_add_internal_dependencies(render_protocol_async lux::engine::render::render_protocol)

    target_link_libraries(render_protocol_async INTERFACE STDEXEC::stdexec lux::engine::core::core_execution)

    component_add_transitive_commands(render_protocol_async
        "find_package(stdexec CONFIG REQUIRED)"
        "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

find_package(lux-cxx CONFIG REQUIRED COMPONENTS core)

add_component(
    STATIC
    COMPONENT_NAME render_commands
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_commands
    SOURCE_FILES src/Command.cpp
)

component_include_directories(
    render_commands
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_commands
    lux::engine::render::render_core
    lux::engine::render::render_data
)

target_link_libraries(render_commands PUBLIC lux::cxx::core)

component_add_transitive_commands(
    render_commands
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS core)"
)

target_compile_features(render_commands PUBLIC cxx_std_20)

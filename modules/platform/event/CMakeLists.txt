find_package(lux-cxx REQUIRED COMPONENTS compile_time)

add_interface_component(
    COMPONENT_NAME event
    NAMESPACE      lux::engine::platform
)

target_link_libraries(
    event
    INTERFACE
		lux::cxx::compile_time
)

component_include_directories(
    event
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)

component_add_transitive_commands(
    event
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

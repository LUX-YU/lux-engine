add_interface_component(
    COMPONENT_NAME toolchain_builtin_content
    NAMESPACE      lux::engine::toolchain
)

lux_classify_target(
    TARGET  toolchain_builtin_content
    LAYER   TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

component_include_directories(
    toolchain_builtin_content
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)

target_link_libraries(
    toolchain_builtin_content
    INTERFACE
        lux::engine::content
        lux::engine::resource::description
)

component_add_transitive_commands(
    toolchain_builtin_content
    "find_package(lux-engine-content REQUIRED COMPONENTS content)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)

add_subdirectory(baker)

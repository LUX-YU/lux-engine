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
        lux::engine::resource::description
)

add_subdirectory(baker)

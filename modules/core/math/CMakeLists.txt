find_package (Eigen3 REQUIRED)

add_interface_component(
    COMPONENT_NAME math
    NAMESPACE      lux::engine::core
)

component_include_directories(
    math
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_link_libraries(
    math
    INTERFACE
    Eigen3::Eigen
)

component_add_transitive_commands(
    math
    "find_package (Eigen3 REQUIRED)"
)

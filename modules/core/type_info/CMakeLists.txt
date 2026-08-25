find_package(lux-cxx CONFIG REQUIRED COMPONENTS compile_time)

add_interface_component(
    COMPONENT_NAME type_info
    NAMESPACE      lux::engine::core
)

component_include_directories(
    type_info
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)

target_link_libraries(
    type_info
    INTERFACE lux::cxx::compile_time
)

component_add_transitive_commands(
    type_info
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  type_info
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

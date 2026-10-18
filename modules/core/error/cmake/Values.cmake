include_guard(GLOBAL)

add_interface_component(
    COMPONENT_NAME error_values
    NAMESPACE lux::engine::core
)

find_package(lux-cxx CONFIG REQUIRED COMPONENTS compile_time)
target_link_libraries(error_values INTERFACE lux::cxx::compile_time)
component_add_transitive_commands(error_values "find_package(lux-cxx CONFIG REQUIRED COMPONENTS compile_time)")

target_include_directories(error_values INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>"
    "$<INSTALL_INTERFACE:include>"
)

install(
    FILES "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/error/Error.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/error/ErrorDescriptor.hpp"
    DESTINATION include/lux/engine/error
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_lua_script_packager ${CMAKE_CURRENT_SOURCE_DIR}/package_lua_script.py)
add_interface_component(
    COMPONENT_NAME lua_script_packager
    NAMESPACE      lux::engine::toolchain
)
target_link_libraries(
    lua_script_packager
    INTERFACE lux::engine::function::script_core
)
component_add_transitive_commands(
    lua_script_packager
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
)

component_add_cmake_scripts(
    lua_script_packager
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux_package_lua_script.cmake
)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux_package_lua_script.cmake)

install(
    PROGRAMS ${_lua_script_packager}
    DESTINATION share/lux-engine-toolchain/lua_script_packager/cmake_scripts
)

install_components(
    PROJECT_NAME lux-engine-toolchain
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::toolchain
    COMPONENTS   lua_script_packager
)

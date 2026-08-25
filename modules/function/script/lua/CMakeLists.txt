find_package(LuaJIT REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

add_component(
    COMPONENT_NAME  script_lua
    NAMESPACE       lux::engine::function
    OUTPUT_NAME     lux_engine_function_script_lua
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Lua.cpp
)

component_include_directories(
    script_lua
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${LUAJIT_INCLUDE_DIRS}
    INSTALL_TIME
        include
    PRIVATE
        pinclude
)

target_compile_definitions(
    script_lua
    PRIVATE
        LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    script_lua
    PUBLIC
        lux::engine::function::script_core
        lux::cxx::compile_time
        sol2::sol2
        LuaJIT::LuaJIT
)

component_add_transitive_commands(
    script_lua
    [[list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")]]
    "find_package(LuaJIT REQUIRED)"
    "find_package(sol2 CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
)

component_add_cmake_scripts(
    script_lua
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_lua_codegen.cmake
)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cmake/template/
    DESTINATION share/lux-engine-function/script_lua/cmake_scripts/template
)

install(
    FILES ${CMAKE_SOURCE_DIR}/cmake/FindLuaJIT.cmake
    DESTINATION share/lux-engine-function/script_lua
)

lux_classify_target(
    TARGET  script_lua
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

set(ENABLE_SCRIPT_LUA_TEST OFF CACHE BOOL "Enable Lua script tests")
if(ENABLE_SCRIPT_LUA_TEST)
    add_subdirectory(test)
endif()

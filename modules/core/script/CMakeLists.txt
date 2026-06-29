## core::script — single, unified script component.
##
## Merges what used to be `core::script` (host + ABI), `core::script_lua`
## (LuaJIT backend) and `core::script_native` (compiled-dll backend) into one
## component. Backends are toggled at configure-time via:
##
##   LUX_SCRIPT_ENABLE_LUA    (default ON)  — pulls LuaJIT
##   LUX_SCRIPT_ENABLE_NATIVE (default ON)  — pulls platform::dynamic_library
##
## Cross-layer dependency on `lux::engine::platform::dynamic_library` is
## expressed via `target_link_libraries` (NOT `component_add_internal_dependencies`,
## which is same-layer only) and a transitive find_package command for downstream
## install consumers.

option(LUX_SCRIPT_ENABLE_LUA    "Enable Lua backend in core::script"    ON)
option(LUX_SCRIPT_ENABLE_NATIVE "Enable Native (compiled-dll) backend in core::script" ON)

set(LUX_SCRIPT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptHost.cpp
)

if(LUX_SCRIPT_ENABLE_LUA)
    find_package(LuaJIT REQUIRED)
    list(APPEND LUX_SCRIPT_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lua/Lua.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lua/LuaBackend.cpp
    )
endif()

if(LUX_SCRIPT_ENABLE_NATIVE)
    list(APPEND LUX_SCRIPT_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/native/NativeBackend.cpp
    )
endif()

add_component(
    COMPONENT_NAME  script
    NAMESPACE       lux::engine::core
    OUTPUT_NAME     lux_engine_core_script
    SOURCE_FILES    ${LUX_SCRIPT_SRCS}
)

component_include_directories(
    script
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED   ${ENTT_INCLUDE_DIRS}
    INSTALL_TIME        include
    PRIVATE             pinclude
)

target_compile_definitions(
    script
    PRIVATE
    LUX_CORE_LIBRARY
)

component_add_internal_dependencies(
    script
    meta
)

target_link_libraries(
    script
    PUBLIC
    lux::cxx::reflection_generator
)

# ---------------------------------------------------------------------------
# Lua backend
# ---------------------------------------------------------------------------
if(LUX_SCRIPT_ENABLE_LUA)
    target_compile_definitions(script PUBLIC LUX_SCRIPT_HAS_LUA=1)
    component_include_directories(
        script
        BUILD_TIME_SHARED ${LUAJIT_INCLUDE_DIRS}
    )
    target_link_libraries(script PUBLIC LuaJIT::LuaJIT)
    component_add_transitive_commands(
        script
        [[list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")]]
        "find_package(LuaJIT REQUIRED)"
    )
    install(
        FILES ${CMAKE_SOURCE_DIR}/cmake/FindLuaJIT.cmake
        DESTINATION share/lux-engine-core/script
    )
endif()

# ---------------------------------------------------------------------------
# Native (compiled-dll) backend — depends on platform::dynamic_library.
# Cross-layer dep: target_link_libraries + transitive find_package, NOT
# component_add_internal_dependencies (same-layer only).
# ---------------------------------------------------------------------------
if(LUX_SCRIPT_ENABLE_NATIVE)
    target_compile_definitions(script PUBLIC LUX_SCRIPT_HAS_NATIVE=1)
    target_link_libraries(
        script
        PRIVATE
        lux::engine::platform::dynamic_library
    )
    component_add_transitive_commands(
        script
        "find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)"
    )
endif()

set(ENABLE_SCRIPT_TEST OFF CACHE BOOL "Enable script test.")
if(ENABLE_SCRIPT_TEST)
    add_subdirectory(test)
endif()

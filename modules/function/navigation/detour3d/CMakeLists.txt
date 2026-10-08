find_package(recastnavigation 1.6.0 CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS binary memory)

add_component(
    COMPONENT_NAME navigation_detour3d
    NAMESPACE      lux::engine::function
    OUTPUT_NAME    lux_engine_function_navigation_detour3d
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/NavigationDetour3D.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Navigation3DBackend.cpp
)

component_include_directories(
    navigation_detour3d
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_compile_definitions(
    navigation_detour3d
    PRIVATE LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    navigation_detour3d
    PUBLIC
        lux::engine::function::navigation
        lux::cxx::memory
    PRIVATE
        lux::cxx::binary
        RecastNavigation::Detour
)

component_add_transitive_commands(
    navigation_detour3d
    "find_package(lux-cxx REQUIRED COMPONENTS binary memory)"
    "find_package(lux-engine-function REQUIRED COMPONENTS navigation)"
)

if(BUILD_TESTING)
    add_executable(navigation_portal_test test/portal.cpp)
    target_link_libraries(navigation_portal_test PRIVATE navigation_detour3d)
    if(MSVC)
        target_compile_options(navigation_portal_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(navigation_portal_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME navigation.portal COMMAND navigation_portal_test)
endif()

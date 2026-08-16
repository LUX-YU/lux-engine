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

lux_classify_target(
    TARGET  navigation_detour3d
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
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

add_subdirectory(test)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_AUTHORING_FLOWFORGE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_AUTHORING_FLOWFORGE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_AUTHORING_FLOWFORGE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/authoring/flowforge/visibility.h
)
add_component(
    COMPONENT_NAME authoring_flowforge
    NAMESPACE      lux::engine::authoring
    OUTPUT_NAME    lux_engine_authoring_flowforge
    SOURCE_FILES src/FlowForgeGraph.cpp
)
component_include_directories(
    authoring_flowforge
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    authoring_flowforge
    PRIVATE LUX_ENGINE_AUTHORING_FLOWFORGE_LIBRARY
)
target_link_libraries(
    authoring_flowforge
    PUBLIC
        lux::engine::simulation::description
        lux::engine::authoring::authoring_script_binding
)
component_add_transitive_commands(
    authoring_flowforge
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_script_binding)"
)
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(_flowforge_product TOOLCHAIN)
else()
    set(_flowforge_product EDITOR)
endif()
lux_classify_target(
    TARGET  authoring_flowforge
    LAYER   AUTHORING
    PRODUCT ${_flowforge_product}
    ROLE    DOMAIN
)
install_components(
    PROJECT_NAME lux-engine-authoring
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::authoring
    COMPONENTS   authoring_flowforge
                 authoring_script_binding
)

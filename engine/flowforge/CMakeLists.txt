find_package(lux-cxx REQUIRED COMPONENTS algorithm container reflection_runtime)

if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(_flowforge_product TOOLCHAIN)
else()
    set(_flowforge_product EDITOR)
endif()

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_FLOWFORGE_GRAPH_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_FLOWFORGE_GRAPH_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_FLOWFORGE_GRAPH_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/flowforge/graph/visibility.h
)

add_component(
    STATIC
    COMPONENT_NAME flowforge_graph
    NAMESPACE      lux::engine::flowforge
    SOURCE_FILES
        src/NodeRegistry.cpp
        src/NodeBase.cpp
        src/ControlNode.cpp
        src/FunctionalNode.cpp
        src/ObjectNode.cpp
        src/ArithmeticNode.cpp
        src/FlowGraph.cpp
        src/StateLayout.cpp
)
component_include_directories(
    flowforge_graph
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    flowforge_graph
    PRIVATE LUX_ENGINE_FLOWFORGE_GRAPH_LIBRARY
)
target_link_libraries(
    flowforge_graph
    PUBLIC
        lux::cxx::container
        lux::cxx::algorithm
        lux::cxx::reflection_runtime
        lux::engine::core::meta
)
component_add_transitive_commands(
    flowforge_graph
    "find_package(lux-cxx REQUIRED COMPONENTS container algorithm reflection_runtime)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)
lux_classify_target(
    TARGET  flowforge_graph
    LAYER   AUTHORING
    PRODUCT ${_flowforge_product}
    ROLE    DOMAIN
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_FLOWFORGE_SCRIPT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_FLOWFORGE_SCRIPT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_FLOWFORGE_SCRIPT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/flowforge/script/visibility.h
)
add_component(
    COMPONENT_NAME flowforge_script
    NAMESPACE      lux::engine::flowforge
    OUTPUT_NAME    lux_engine_flowforge_script
    SOURCE_FILES src/script/ScriptGraph.cpp
)
component_include_directories(
    flowforge_script
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    flowforge_script
    PRIVATE LUX_ENGINE_FLOWFORGE_SCRIPT_LIBRARY
)
target_link_libraries(
    flowforge_script
    PUBLIC lux::engine::simulation::simulation_script
)
component_add_transitive_commands(
    flowforge_script
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
)
lux_classify_target(
    TARGET  flowforge_script
    LAYER   AUTHORING
    PRODUCT ${_flowforge_product}
    ROLE    DOMAIN
)

option(LUX_ENABLE_FLOWFORGE_GRAPH_TESTS
    "Build the canonical FlowForge graph-model tests" OFF)
if(LUX_ENABLE_FLOWFORGE_GRAPH_TESTS)
    add_subdirectory(test)
endif()

add_subdirectory(compiler)

set(LUX_FLOWFORGE_COMPONENTS
    flowforge_graph
    flowforge_script
    flowforge_script_compiler
)
if(TARGET flowforge_compiler)
    list(APPEND LUX_FLOWFORGE_COMPONENTS flowforge_compiler)
endif()

install_components(
    PROJECT_NAME lux-engine-flowforge
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::flowforge
    COMPONENTS   ${LUX_FLOWFORGE_COMPONENTS}
)

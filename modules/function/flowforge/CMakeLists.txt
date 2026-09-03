find_package(lux-cxx REQUIRED COMPONENTS algorithm container reflection_runtime)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_FLOWFORGE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_FLOWFORGE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_FLOWFORGE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/flowforge/visibility.h
)

add_component(
    STATIC
    COMPONENT_NAME flowforge
    NAMESPACE      lux::engine::function
    OUTPUT_NAME    lux_engine_function_flowforge
    SOURCE_FILES
        src/NodeRegistry.cpp
        src/NodeBase.cpp
        src/ControlNode.cpp
        src/FunctionalNode.cpp
        src/ObjectNode.cpp
        src/ArithmeticNode.cpp
        src/FlowGraph.cpp
        src/StateLayout.cpp
        src/script/ScriptGraph.cpp
        src/script/ScriptAbilityCatalog.cpp
        src/script/ScriptAbilityNode.cpp
)

component_include_directories(
    flowforge
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    flowforge
    PRIVATE LUX_ENGINE_FLOWFORGE_LIBRARY
    PUBLIC LUX_ENGINE_FLOWFORGE_DLL_DISABLE
)

target_link_libraries(
    flowforge
    PUBLIC
        lux::engine::function::graph
        lux::cxx::container
        lux::cxx::algorithm
        lux::cxx::reflection_runtime
        lux::engine::core::meta
        lux::engine::function::script_core
        lux::engine::resource::description
)

component_add_transitive_commands(
    flowforge
    "find_package(lux-cxx REQUIRED COMPONENTS container algorithm reflection_runtime)"
    "find_package(lux-engine-function REQUIRED COMPONENTS graph)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)

lux_classify_target(
    TARGET  flowforge
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

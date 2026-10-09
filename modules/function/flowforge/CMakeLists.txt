find_package(lux-cxx REQUIRED COMPONENTS algorithm container reflection_runtime)
find_package(tomlplusplus CONFIG REQUIRED)

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
        src/NativeCallDefinition.cpp
        src/FlowNodePayload.cpp
        src/ScalarLowering.cpp
        src/NodeRegistry.cpp
        src/NodeBase.cpp
        src/ControlNode.cpp
        src/FunctionalNode.cpp
        src/ObjectNode.cpp
        src/ArithmeticNode.cpp
        src/FlowGraph.cpp
        src/FlowSource.cpp
        src/FlowSourceGraph.cpp
        src/StateLayout.cpp
        src/script/ScriptGraph.cpp
        src/script/ScriptAbilityCatalog.cpp
        src/script/ScriptAbilityNode.cpp
        src/script/ScriptEventAwaitNode.cpp
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
        lux::engine::core::object
        lux::engine::function::script_core
        lux::engine::resource::description
)
target_compile_definitions(flowforge PRIVATE TOML_EXCEPTIONS=0 TOML_HEADER_ONLY=1)

target_include_directories(flowforge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${CMAKE_CURRENT_SOURCE_DIR}/sinclude)

target_include_directories(flowforge PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)

component_add_transitive_commands(
    flowforge
    "find_package(lux-cxx REQUIRED COMPONENTS container algorithm reflection_runtime)"
    "find_package(lux-engine-function REQUIRED COMPONENTS graph)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta object)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)

if(BUILD_TESTING)
    add_executable(flow_constant_default_test test/constant_default.cpp)

    target_link_libraries(flow_constant_default_test PRIVATE lux::engine::function::flowforge)

    add_test(NAME flow.constant_default COMMAND flow_constant_default_test)

    add_executable(flow_ability_description_test test/ability_description.cpp)

    target_link_libraries(flow_ability_description_test PRIVATE lux::engine::function::flowforge)

    add_test(NAME flow.ability_catalog_ownership COMMAND flow_ability_description_test catalog)

    add_test(NAME flow.ability_node_ownership COMMAND flow_ability_description_test node)

    add_library(flow_ability_description_plugin SHARED test/ability_plugin.cpp)

    target_link_libraries(flow_ability_description_plugin PRIVATE lux::engine::function::flowforge)

    add_executable(flow_ability_plugin_test test/ability_plugin_test.cpp)

    target_link_libraries(
        flow_ability_plugin_test
        PRIVATE lux::engine::function::flowforge lux::engine::platform::dynamic_library
    )

    add_dependencies(flow_ability_plugin_test flow_ability_description_plugin)

    add_test(
        NAME flow.ability_description_plugin
        COMMAND flow_ability_plugin_test "$<TARGET_FILE:flow_ability_description_plugin>"
    )
endif()

if(BUILD_TESTING)
    add_executable(flow_node_payload_test test/node_payload.cpp)

    target_link_libraries(flow_node_payload_test PRIVATE lux::engine::function::flowforge)

    add_test(NAME flow.node_payload COMMAND flow_node_payload_test)

    add_library(flow_node_payload_plugin MODULE test/node_payload_plugin.cpp)

    target_link_libraries(flow_node_payload_plugin PRIVATE lux::engine::function::flowforge)

    add_executable(flow_node_payload_plugin_test test/node_payload_plugin_test.cpp)

    target_link_libraries(
        flow_node_payload_plugin_test
        PRIVATE lux::engine::function::flowforge lux::engine::platform::dynamic_library
    )

    add_dependencies(flow_node_payload_plugin_test flow_node_payload_plugin)

    add_test(
        NAME flow.node_payload_plugin
        COMMAND flow_node_payload_plugin_test "$<TARGET_FILE:flow_node_payload_plugin>"
    )
endif()

if(BUILD_TESTING)
    add_executable(flow_native_definition_test test/native_definition.cpp)

    target_link_libraries(flow_native_definition_test PRIVATE lux::engine::function::flowforge)

    add_test(NAME flow.native_definition COMMAND flow_native_definition_test)
endif()

if(BUILD_TESTING)
    add_library(flow_native_definition_plugin MODULE test/native_definition_plugin.cpp)

    target_link_libraries(flow_native_definition_plugin PRIVATE lux::engine::function::flowforge)

    add_executable(flow_native_definition_plugin_test test/native_definition_plugin_test.cpp)

    target_link_libraries(
        flow_native_definition_plugin_test
        PRIVATE lux::engine::function::flowforge lux::engine::platform::dynamic_library
    )

    add_dependencies(flow_native_definition_plugin_test flow_native_definition_plugin)

    add_test(
        NAME flow.native_definition_plugin
        COMMAND flow_native_definition_plugin_test "$<TARGET_FILE:flow_native_definition_plugin>"
    )
endif()

if(BUILD_TESTING)
    add_executable(flow_structure_access_test test/structure_access.cpp)

    target_link_libraries(flow_structure_access_test PRIVATE lux::engine::function::flowforge)

    add_test(NAME flow.structure_access COMMAND flow_structure_access_test)
endif()

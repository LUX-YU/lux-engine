generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_MATERIAL_GRAPH_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_MATERIAL_GRAPH_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_MATERIAL_GRAPH_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/material/graph/visibility.h
)

find_package(tomlplusplus CONFIG REQUIRED)

add_component(
    COMPONENT_NAME material_graph
    NAMESPACE      lux::engine::function
    OUTPUT_NAME    lux_engine_function_material_graph
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialGraph.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialLowering.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialMath.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/BuiltinMaterialNodes.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/BuiltinMaterialCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialSource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderIR.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialNodePayload.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialNodeCatalog.cpp
)

component_include_directories(
    material_graph
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_include_directories(material_graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)

target_compile_definitions(
    material_graph
    PRIVATE LUX_ENGINE_MATERIAL_GRAPH_LIBRARY TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0
)

target_include_directories(
    material_graph
    PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
    material_graph
    PUBLIC
        lux::engine::function::graph
        lux::engine::resource::description
        lux::engine::resource::identity
        lux::engine::core::object
)

component_add_transitive_commands(
    material_graph
    "find_package(lux-engine-function REQUIRED COMPONENTS graph)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity description)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
)

if(BUILD_TESTING)
    add_executable(material_registered_graph_test test/registered_graph.cpp)

    target_link_libraries(material_registered_graph_test PRIVATE lux::engine::function::material_graph)

    add_test(
        NAME material.registered_graph
        COMMAND material_registered_graph_test "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/all_nodes_v1.luxmaterial"
    )

    add_executable(material_authoring_schema_test test/authoring_schema.cpp)

    target_link_libraries(material_authoring_schema_test PRIVATE lux::engine::function::material_graph)

    add_test(NAME material.authoring_schema COMMAND material_authoring_schema_test)

    add_executable(material_builtin_nodes_test test/builtin_nodes.cpp)

    target_link_libraries(material_builtin_nodes_test PRIVATE lux::engine::function::material_graph)

    add_test(NAME material.builtin_nodes COMMAND material_builtin_nodes_test)

    add_executable(material_math_registration_test test/math_registration.cpp)

    target_link_libraries(material_math_registration_test PRIVATE lux::engine::function::material_graph)

    add_test(NAME material.math_registration COMMAND material_math_registration_test)

    add_executable(material_lowering_test test/lowering.cpp)

    target_link_libraries(material_lowering_test PRIVATE lux::engine::function::material_graph)

    add_test(NAME material.lowering COMMAND material_lowering_test)

    add_executable(material_node_identity_test test/node_identity.cpp)

    target_link_libraries(material_node_identity_test PRIVATE lux::engine::function::material_graph)

    add_test(NAME material.node_identity COMMAND material_node_identity_test)

    add_executable(material_shader_ir_test test/shader_ir.cpp)

    target_link_libraries(material_shader_ir_test PRIVATE lux::engine::function::material_graph)

    add_test(NAME material.shader_ir COMMAND material_shader_ir_test)

    add_executable(material_node_catalog_test test/node_catalog.cpp)

    target_link_libraries(material_node_catalog_test PRIVATE lux::engine::function::material_graph)

    add_test(NAME material.node_catalog COMMAND material_node_catalog_test)

    add_library(material_node_plugin MODULE test/node_plugin.cpp)

    target_link_libraries(material_node_plugin PRIVATE lux::engine::function::material_graph)

    add_executable(material_node_plugin_test test/node_plugin_test.cpp)

    target_link_libraries(
        material_node_plugin_test
        PRIVATE lux::engine::function::material_graph lux::engine::platform::dynamic_library
    )

    add_dependencies(material_node_plugin_test material_node_plugin)

    add_test(NAME material.node_plugin COMMAND material_node_plugin_test $<TARGET_FILE:material_node_plugin>)
endif()

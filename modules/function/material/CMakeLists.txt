generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_MATERIAL_GRAPH_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_MATERIAL_GRAPH_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_MATERIAL_GRAPH_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/material/graph/visibility.h
)

add_component(
    COMPONENT_NAME material_graph
    NAMESPACE      lux::engine::function
    OUTPUT_NAME    lux_engine_function_material_graph
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Node.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialGraph.cpp
)

component_include_directories(
    material_graph
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    material_graph
    PRIVATE LUX_ENGINE_MATERIAL_GRAPH_LIBRARY
)

target_link_libraries(
    material_graph
    PUBLIC
        lux::engine::resource::description
        lux::engine::resource::identity
)

component_add_transitive_commands(
    material_graph
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity description)"
)

lux_classify_target(
    TARGET  material_graph
    LAYER   FUNCTION
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

add_executable(
    material_graph_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/material_graph_test.cpp
)
target_link_libraries(
    material_graph_test
    PRIVATE lux::engine::function::material_graph
)
if(MSVC)
    target_compile_options(material_graph_test PRIVATE /UNDEBUG)
else()
    target_compile_options(material_graph_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  material_graph_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME material_graph_test COMMAND material_graph_test)

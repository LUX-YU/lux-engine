find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_FUNCTION_GRAPH_LIBRARY
    PUBLIC_MACRO_NAME       LUX_FUNCTION_GRAPH_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_FUNCTION_GRAPH_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/function/graph/visibility.h
)

add_component(
    COMPONENT_NAME graph
    NAMESPACE      lux::engine::function
    OUTPUT_NAME    lux_engine_function_graph
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphLayout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphTopology.cpp
)

component_include_directories(
    graph
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(graph PRIVATE LUX_FUNCTION_GRAPH_LIBRARY)
target_link_libraries(graph PUBLIC lux::cxx::compile_time)
component_add_transitive_commands(
    graph
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  graph
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(graph_source_test test/graph_source_test.cpp)
target_link_libraries(graph_source_test PRIVATE lux::engine::function::graph)
if(MSVC)
    target_compile_options(graph_source_test PRIVATE /UNDEBUG)
else()
    target_compile_options(graph_source_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  graph_source_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME graph_source_test COMMAND graph_source_test)

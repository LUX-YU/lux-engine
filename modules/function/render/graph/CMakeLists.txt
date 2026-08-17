add_component(
    COMPONENT_NAME render_graph
    NAMESPACE      lux::engine::function
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DependencyAnalyzer.cpp
)

component_include_directories(
    render_graph
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    render_graph
    PRIVATE
        LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    render_graph
    PUBLIC
        lux::engine::platform::common
)

component_add_transitive_commands(
    render_graph
    "find_package(lux-engine-platform REQUIRED COMPONENTS common)"
)

lux_classify_target(
    TARGET  render_graph
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    render_graph_dependency_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/DependencyAnalyzerTest.cpp
)
target_link_libraries(
    render_graph_dependency_test
    PRIVATE
        render_graph
)

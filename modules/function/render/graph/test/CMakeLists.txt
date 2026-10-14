add_executable(render_graph_test Graph.cpp)

target_link_libraries(render_graph_test PRIVATE render_graph)

foreach(group authoring ranges versions topology conditional_history lifetimes oracle)
    add_test(NAME render.graph.${group} COMMAND render_graph_test ${group})
    set_tests_properties(render.graph.${group} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(render_graph_cost Cost.cpp)

target_link_libraries(render_graph_cost PRIVATE render_graph)

foreach(header Graph Builder Analysis)
    string(CONCAT probe
        "#include <lux/engine/render/graph/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Graph\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp" CONTENT "${probe}")

    add_library(render_graph_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp")

    target_link_libraries(render_graph_header_${header} PRIVATE render_graph)
endforeach()

get_target_property(graph_links render_graph INTERFACE_LINK_LIBRARIES)
if(NOT graph_links STREQUAL "lux::engine::render::render_core")
    message(FATAL_ERROR "Unexpected Graph public dependency: ${graph_links}")
endif()

file(
    GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/graph-dependencies.txt"
    CONTENT "public_links=$<TARGET_PROPERTY:render_graph,INTERFACE_LINK_LIBRARIES>\n"
)

add_test(
    NAME render.graph.installed
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/installed"
        "-DDEPENDENCY_DIR=${lux-cxx_DIR}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONFIG=$<CONFIG>"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../../core/test/Installed.cmake"
)

add_executable(render_graph_test Graph.cpp)

target_link_libraries(render_graph_test PRIVATE lux::engine::render::render_graph)

foreach(case IN ITEMS declarations hazards lifetimes legacy bindings reuse generated)
    add_test(NAME render.graph.${case} COMMAND render_graph_test ${case})
endforeach()

foreach(header IN ITEMS Definition Plan Bindings)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp")
    file(WRITE "${source}" "#include <lux/engine/render/graph/${header}.hpp>\n")
    add_library(render_graph_header_${header} OBJECT "${source}")
    target_link_libraries(render_graph_header_${header} PRIVATE lux::engine::render::render_graph)
endforeach()

add_executable(render_graph_probe_control Probe.cpp)

target_link_libraries(render_graph_probe_control PRIVATE lux::engine::render::render_graph)

add_test(NAME render.graph.probe_control COMMAND render_graph_probe_control)

foreach(probe IN ITEMS ID TEMPORARY VULKAN SCENE RUNTIME TRANSPORT LEGACY EDITOR FEATURE)
    add_executable(render_graph_reject_${probe} EXCLUDE_FROM_ALL Probe.cpp)
    target_compile_definitions(render_graph_reject_${probe} PRIVATE LUX_PROBE_${probe})
    target_link_libraries(render_graph_reject_${probe} PRIVATE lux::engine::render::render_graph)
    add_test(
        NAME render.graph.reject_${probe}
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DPROBE=${probe}"
            "-DMSVC_PROBE=${MSVC}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/RejectCompile.cmake"
    )
    set_tests_properties(render.graph.reject_${probe} PROPERTIES RUN_SERIAL TRUE)
endforeach()

add_executable(render_graph_benchmark Benchmark.cpp)

target_link_libraries(render_graph_benchmark PRIVATE lux::engine::render::render_graph)

add_test(NAME render.graph.binding_allocation COMMAND render_graph_benchmark)

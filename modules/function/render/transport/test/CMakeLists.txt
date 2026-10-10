set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/RenderTransportCodegen.cmake")

add_executable(render_transport_test Contracts.cpp Concurrency.cpp)

target_link_libraries(render_transport_test PRIVATE lux::engine::render::render_transport)

# Only generic annotation macros are codegen input; no type_info runtime target.
get_filename_component(annotation_include "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include" ABSOLUTE)

target_include_directories(render_transport_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${annotation_include}")

render_transport_operations(render_transport_test "${CMAKE_CURRENT_SOURCE_DIR}/TestOps.hpp")

foreach(test IN ITEMS contracts ownership replies threads uploads stop causality)
    add_test(NAME render.transport.${test} COMMAND render_transport_test ${test})
    set_tests_properties(render.transport.${test} PROPERTIES TIMEOUT 45)
endforeach()

add_executable(render_transport_probe_control Probe.cpp)

target_link_libraries(render_transport_probe_control PRIVATE lux::engine::render::render_transport)

target_include_directories(
    render_transport_probe_control
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${annotation_include}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

add_dependencies(render_transport_probe_control render_transport_test_operations_generate)

add_test(NAME render.transport.probe_control COMMAND render_transport_probe_control)

foreach(probe IN ITEMS LANE KIND PAYLOAD VULKAN RUNTIME SCENE)
    set(target "render_transport_reject_${probe}")
    add_executable(${target} EXCLUDE_FROM_ALL Probe.cpp)
    target_link_libraries(${target} PRIVATE lux::engine::render::render_transport)
    target_include_directories(
        ${target}
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${annotation_include}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
    target_compile_definitions(${target} PRIVATE "LUX_REJECT_${probe}")
    add_dependencies(${target} render_transport_test_operations_generate)
    if(probe MATCHES "LANE|KIND|PAYLOAD")
        set(expected "C2672|no matching|constraints not satisfied")
    else()
        set(expected "C1083|No such file|file not found")
    endif()
    add_test(
        NAME render.transport.reject_${probe}
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DTARGET_NAME=${target}"
            "-DEXPECTED=${expected}" -P "${CMAKE_CURRENT_SOURCE_DIR}/RejectCompile.cmake"
    )
    set_tests_properties(render.transport.reject_${probe} PROPERTIES RUN_SERIAL TRUE TIMEOUT 45)
endforeach()

add_executable(render_transport_reject_POINTER EXCLUDE_FROM_ALL InvalidSchema.cpp)

target_link_libraries(render_transport_reject_POINTER PRIVATE lux::engine::render::render_transport)

target_include_directories(
    render_transport_reject_POINTER
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${annotation_include}"
)

render_transport_operations(render_transport_reject_POINTER "${CMAKE_CURRENT_SOURCE_DIR}/BadOps.hpp")

add_test(
    NAME render.transport.reject_POINTER
    COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DTARGET_NAME=render_transport_reject_POINTER"
        "-DEXPECTED=Transport payload must contain only owned value fields"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RejectCompile.cmake"
)

set_tests_properties(render.transport.reject_POINTER PROPERTIES RUN_SERIAL TRUE TIMEOUT 45)

add_executable(render_transport_benchmark Benchmark.cpp)

target_link_libraries(render_transport_benchmark PRIVATE lux::engine::render::render_transport)

target_include_directories(
    render_transport_benchmark
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${annotation_include}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

add_dependencies(render_transport_benchmark render_transport_test_operations_generate)

foreach(header IN ITEMS Error Operation Routes Replies Packet Transport)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp")
    file(WRITE "${source}" "#include <lux/engine/render/transport/${header}.hpp>\n")
    add_library(render_transport_header_${header} OBJECT "${source}")
    target_link_libraries(render_transport_header_${header} PRIVATE lux::engine::render::render_transport)
endforeach()

add_executable(render_transport_test Contracts.cpp Concurrency.cpp Edges.cpp)

target_link_libraries(render_transport_test PRIVATE render_transport)

foreach(test routes batch replies bulk generation spsc mpmc races ordering edges owners active-stop)
    add_test(NAME render.transport.${test} COMMAND render_transport_test ${test})
    set_tests_properties(render.transport.${test} PROPERTIES TIMEOUT 45)
endforeach()

add_executable(render_transport_allocation_test Allocation.cpp)

target_link_libraries(render_transport_allocation_test PRIVATE render_transport)

add_test(NAME render.transport.allocation COMMAND render_transport_allocation_test)

set_tests_properties(render.transport.allocation PROPERTIES TIMEOUT 45)

foreach(header Operation Routes Replies Transport)
    string(CONCAT probe
        "#include <lux/engine/render/transport/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Transport\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT "${probe}")

    add_library(render_transport_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(render_transport_header_${header} PRIVATE render_transport)
endforeach()

add_test(
    NAME render.transport.installed
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

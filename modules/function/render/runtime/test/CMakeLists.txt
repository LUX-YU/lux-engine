add_executable(render_runtime_test Runtime.cpp)

target_include_directories(render_runtime_test PRIVATE ../../native/vulkan/test)
target_link_libraries(render_runtime_test PRIVATE render_runtime)
set_target_properties(render_runtime_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.control COMMAND render_runtime_test)
set_tests_properties(render.runtime.control PROPERTIES TIMEOUT 90)

add_executable(render_runtime_lifetime_test Lifetimes.cpp)
target_include_directories(render_runtime_lifetime_test PRIVATE ../../native/vulkan/test)
target_link_libraries(render_runtime_lifetime_test PRIVATE render_runtime)
set_target_properties(render_runtime_lifetime_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME render.runtime.lifetime COMMAND render_runtime_lifetime_test)
set_tests_properties(render.runtime.lifetime PROPERTIES TIMEOUT 90)

add_test(
    NAME render.runtime.installed
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

foreach(header RenderClient RenderServer)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT
        "#include <lux/engine/render/runtime/${header}.hpp>\n#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native leak\n#endif\nstatic_assert(__cplusplus >= 202002L);\n"
    )
    add_library(render_runtime_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")
    # Both neutral headers must compile without Native's include paths.
    target_link_libraries(render_runtime_header_${header} PRIVATE render_client render_features)
endforeach()

add_executable(render_native_test Foundation.cpp)

target_link_libraries(render_native_test PRIVATE render_native_vulkan)

add_test(NAME render.native.foundation COMMAND render_native_test)

set_tests_properties(render.native.foundation PROPERTIES TIMEOUT 60)

# The same source algorithms with compile-time fault hooks, never used by production.
add_library(render_native_instrumented STATIC)

foreach(source IN LISTS LUX_NATIVE_SOURCES)
    target_sources(render_native_instrumented PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../${source}")
endforeach()

target_compile_definitions(render_native_instrumented PRIVATE LUX_VULKAN_TEST_SEAM)

target_include_directories(render_native_instrumented
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pinclude" "${LUX_VMA_INCLUDE_DIR}"
)

target_link_libraries(render_native_instrumented PUBLIC render_core Vulkan::Vulkan)

add_executable(render_native_fault_test Foundation.cpp)

target_compile_definitions(render_native_fault_test PRIVATE LUX_VULKAN_TEST_SEAM)

target_link_libraries(render_native_fault_test PRIVATE render_native_instrumented)

add_test(NAME render.native.failures COMMAND render_native_fault_test)

set_tests_properties(render.native.failures PROPERTIES TIMEOUT 90)

foreach(header Error DeviceSession device/Device memory/Memory transfer/Submission retirement/Retirement)
    string(REPLACE "/" "_" target "${header}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${target}.cpp"
        CONTENT "#include <lux/engine/render/native/vulkan/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
    )
    add_library(render_native_header_${target} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${target}.cpp")
    target_link_libraries(render_native_header_${target} PRIVATE render_native_vulkan)
endforeach()

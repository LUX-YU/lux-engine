add_executable(render_native_test Foundation.cpp)

target_link_libraries(render_native_test PRIVATE render_native_vulkan)

add_test(NAME render.native.foundation COMMAND render_native_test)

set_tests_properties(render.native.foundation PROPERTIES TIMEOUT 60)

add_executable(render_compiler_test Compiler.cpp)

target_link_libraries(render_compiler_test PRIVATE render_native_vulkan)

add_test(NAME render.native.compiler COMMAND render_compiler_test)

set_tests_properties(render.native.compiler PROPERTIES TIMEOUT 60)

add_executable(render_execution_test Execution.cpp Allocation.cpp)
target_link_libraries(render_execution_test PRIVATE render_native_vulkan)
add_test(NAME render.native.execution COMMAND render_execution_test)
set_tests_properties(render.native.execution PROPERTIES TIMEOUT 60)

# The same source algorithms with compile-time fault hooks, never used by production.
add_library(render_native_instrumented STATIC)

foreach(source IN LISTS LUX_NATIVE_SOURCES)
    target_sources(render_native_instrumented PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../${source}")
endforeach()

add_test(
    NAME render.native.installed
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/installed"
        "-DDEPENDENCY_DIR=${lux-cxx_DIR}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONFIG=$<CONFIG>"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/test/Installed.cmake"
)

target_compile_definitions(render_native_instrumented PRIVATE LUX_VULKAN_TEST_SEAM)

target_include_directories(render_native_instrumented
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pinclude" "${LUX_VMA_INCLUDE_DIR}"
)

target_link_libraries(render_native_instrumented PUBLIC render_features Vulkan::Vulkan
    PRIVATE lux_render_spirv_cross_c_shared lux_render_SPIRV_Tools_shared
)

add_executable(render_native_fault_test Foundation.cpp)

target_compile_definitions(render_native_fault_test PRIVATE LUX_VULKAN_TEST_SEAM)

target_link_libraries(render_native_fault_test PRIVATE render_native_instrumented)

add_test(NAME render.native.failures COMMAND render_native_fault_test)

set_tests_properties(render.native.failures PROPERTIES TIMEOUT 90)

foreach(header Program Error DeviceSession CompiledGraph ExecutionContext device/Device memory/Memory
    transfer/Submission transfer/TextureUpload transfer/UploadOptions texture/TextureStorage retirement/Retirement
)
    string(REPLACE "/" "_" target "${header}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${target}.cpp"
        CONTENT "#include <lux/engine/render/native/vulkan/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
    )
    add_library(render_native_header_${target} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${target}.cpp")
    target_link_libraries(render_native_header_${target} PRIVATE render_native_vulkan)
endforeach()

add_executable(render_memory_cost MemoryCost.cpp Allocation.cpp)

target_link_libraries(render_memory_cost PRIVATE render_native_vulkan)
add_test(NAME render.native.memory_cost COMMAND render_memory_cost)
set_tests_properties(render.native.memory_cost PROPERTIES TIMEOUT 60)

add_executable(render_binding_plan_test BindingPlan.cpp)

target_link_libraries(render_binding_plan_test PRIVATE render_native_instrumented)
add_test(NAME render.native.binding_plan COMMAND render_binding_plan_test)
set_tests_properties(render.native.binding_plan PROPERTIES TIMEOUT 60)

add_executable(render_transfer_test Transfer.cpp)
target_link_libraries(render_transfer_test PRIVATE render_native_vulkan)
add_test(NAME render.native.transfer COMMAND render_transfer_test)
set_tests_properties(render.native.transfer PROPERTIES TIMEOUT 60)

add_executable(render_texture_upload_test TextureUpload.cpp)
target_link_libraries(render_texture_upload_test PRIVATE render_native_vulkan)
add_test(NAME render.native.texture_upload COMMAND render_texture_upload_test)
set_tests_properties(render.native.texture_upload PROPERTIES TIMEOUT 90)

add_executable(render_texture_storage_test TextureStorage.cpp)

target_link_libraries(render_texture_storage_test PRIVATE render_native_instrumented)

add_test(NAME render.native.texture_storage COMMAND render_texture_storage_test)

set_tests_properties(render.native.texture_storage PROPERTIES TIMEOUT 60)

add_executable(render_texture_sampling_test TextureSampling.cpp Allocation.cpp)

target_link_libraries(render_texture_sampling_test PRIVATE render_native_instrumented)

add_test(NAME render.native.texture_sampling COMMAND render_texture_sampling_test)

set_tests_properties(render.native.texture_sampling PROPERTIES TIMEOUT 90)

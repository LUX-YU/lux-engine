add_executable(render_native_test Foundation.cpp)

target_link_libraries(render_native_test PRIVATE render_native_vulkan)

add_test(NAME render.native.foundation COMMAND render_native_test)

set_tests_properties(render.native.foundation PROPERTIES TIMEOUT 60)

add_executable(render_compiler_test Compiler.cpp)

target_link_libraries(render_compiler_test PRIVATE render_native_vulkan render_features)

add_test(NAME render.native.compiler COMMAND render_compiler_test)

set_tests_properties(render.native.compiler PROPERTIES TIMEOUT 60)

add_executable(render_execution_test Execution.cpp Allocation.cpp)
target_link_libraries(render_execution_test PRIVATE render_native_vulkan render_features)
add_test(NAME render.native.execution COMMAND render_execution_test)
set_tests_properties(render.native.execution PROPERTIES TIMEOUT 60)

# The same source algorithms with compile-time fault hooks, never used by production.
add_library(render_native_instrumented STATIC)

foreach(source IN LISTS LUX_NATIVE_SOURCES LUX_NATIVE_EXTENSION_SOURCES)
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
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pinclude" "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude" "${LUX_VMA_INCLUDE_DIR}"
)

target_link_libraries(render_native_instrumented PUBLIC render_features Vulkan::Vulkan
    PRIVATE lux_render_spirv_cross_c_shared lux_render_SPIRV_Tools_shared lux::engine::platform::gapi
)

add_executable(render_native_fault_test Foundation.cpp)

target_compile_definitions(render_native_fault_test PRIVATE LUX_VULKAN_TEST_SEAM)

target_link_libraries(render_native_fault_test PRIVATE render_native_instrumented)

add_test(NAME render.native.failures COMMAND render_native_fault_test)

set_tests_properties(render.native.failures PROPERTIES TIMEOUT 90)

add_executable(render_resource_domains_test ResourceDomains.cpp)

target_link_libraries(render_resource_domains_test PRIVATE render_native_instrumented)

add_test(NAME render.native.resource_domains COMMAND render_resource_domains_test)

set_tests_properties(render.native.resource_domains PROPERTIES TIMEOUT 60)

add_executable(render_buffer_transfer_test BufferTransfer.cpp)

target_link_libraries(render_buffer_transfer_test PRIVATE render_native_instrumented)

add_test(NAME render.native.buffer_transfer COMMAND render_buffer_transfer_test)

set_tests_properties(render.native.buffer_transfer PROPERTIES TIMEOUT 60)

foreach(header Error DeviceSession CompiledGraph ExecutionContext StorageBinding device/Device device/DeviceOptions memory/Memory
    Readback transfer/Submission transfer/Transfer transfer/GpuWork
    texture/TextureStorage texture/TextureCommands Registration retirement/Retirement
)
    string(REPLACE "/" "_" target "${header}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${target}.cpp"
        CONTENT "#include <lux/engine/render/native/vulkan/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
    )
    add_library(render_native_header_${target} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${target}.cpp")
    if(header MATCHES "^(texture/|Registration$)")
        target_link_libraries(render_native_header_${target} PRIVATE render_native_vulkan_extensions)
    else()
        target_link_libraries(render_native_header_${target} PRIVATE render_native_vulkan)
    endif()
endforeach()

add_executable(render_queue_callbacks_test QueueCallbacks.cpp)

target_link_libraries(render_queue_callbacks_test PRIVATE render_native_instrumented)

add_test(NAME render.native.queue_callbacks COMMAND render_queue_callbacks_test)

set_tests_properties(render.native.queue_callbacks PROPERTIES TIMEOUT 90)

add_executable(render_last_use_test LastUse.cpp)

target_link_libraries(render_last_use_test PRIVATE render_native_instrumented lux::engine::core::core_execution)

set_target_properties(render_last_use_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.native.last_use COMMAND render_last_use_test)

set_tests_properties(render.native.last_use PROPERTIES TIMEOUT 90)

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
target_link_libraries(render_texture_upload_test PRIVATE render_native_vulkan_extensions)
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

add_executable(render_storage_bindings_test StorageBindings.cpp Allocation.cpp)

target_link_libraries(render_storage_bindings_test PRIVATE render_native_vulkan_extensions)

add_test(NAME render.native.storage_bindings COMMAND render_storage_bindings_test)

set_tests_properties(render.native.storage_bindings PROPERTIES TIMEOUT 60)

foreach(target render_buffer_transfer_test render_texture_upload_test render_texture_storage_test
    render_texture_sampling_test render_storage_bindings_test
)
    target_link_libraries(${target} PRIVATE lux::engine::core::core_execution)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
endforeach()

add_executable(render_explicit_factory_test ExplicitFactory.cpp)

target_link_libraries(render_explicit_factory_test PRIVATE render_native_vulkan)

add_test(NAME render.native.explicit_factory COMMAND render_explicit_factory_test)

set_tests_properties(render.native.explicit_factory PROPERTIES TIMEOUT 60)

add_executable(render_direct_graph_test DirectGraph.cpp)

target_link_libraries(render_direct_graph_test PRIVATE render_native_vulkan)

add_test(NAME render.native.direct_graph COMMAND render_direct_graph_test)

set_tests_properties(render.native.direct_graph PROPERTIES TIMEOUT 60)

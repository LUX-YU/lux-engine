add_executable(render_runtime_test Runtime.cpp)

add_executable(render_attachment_indices_test AttachmentIndices.cpp)

target_include_directories(render_attachment_indices_test PRIVATE ../pinclude)
target_link_libraries(render_attachment_indices_test PRIVATE render_resource)

add_test(NAME render.runtime.attachment_indices COMMAND render_attachment_indices_test)

set_tests_properties(render.runtime.attachment_indices PROPERTIES TIMEOUT 30)

add_executable(render_typed_client_test TypedClient.cpp ../../native/vulkan/test/Allocation.cpp ${LUX_RUNTIME_SOURCES})
target_compile_definitions(render_typed_client_test PRIVATE LUX_RUNTIME_RESPONSE_COST_SEAM)
target_include_directories(render_typed_client_test PRIVATE ../pinclude ../../native/vulkan/test)
target_include_directories(render_typed_client_test PRIVATE ../../../../platform/ipc/pinclude)

target_link_libraries(render_typed_client_test PRIVATE render_runtime)

set_target_properties(render_typed_client_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.typed_client COMMAND render_typed_client_test)

set_tests_properties(render.runtime.typed_client PROPERTIES TIMEOUT 30)

add_executable(render_typed_batch_test TypedBatch.cpp ../../native/vulkan/test/Allocation.cpp)

target_include_directories(render_typed_batch_test PRIVATE ../../native/vulkan/test)

target_link_libraries(render_typed_batch_test PRIVATE render_runtime lux::engine::core::core_execution)

set_target_properties(render_typed_batch_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.typed_batch COMMAND render_typed_batch_test)

set_tests_properties(render.runtime.typed_batch PROPERTIES TIMEOUT 90)

add_library(render_client_dll_plugin SHARED ClientDllPlugin.cpp)

target_link_libraries(render_client_dll_plugin PRIVATE render_commands lux::engine::platform::platform_ipc)

set_target_properties(render_client_dll_plugin PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin"
)

add_executable(render_client_dll_test ClientDll.cpp)

target_link_libraries(render_client_dll_test PRIVATE render_commands lux::engine::platform::platform_ipc)

add_dependencies(render_client_dll_test render_client_dll_plugin)

set_target_properties(render_client_dll_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.client_dll COMMAND render_client_dll_test)

set_tests_properties(render.runtime.client_dll PROPERTIES TIMEOUT 30)

add_library(render_server_dll_plugin SHARED ServerDllPlugin.cpp)

target_link_libraries(render_server_dll_plugin PRIVATE render_runtime)

set_target_properties(render_server_dll_plugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_executable(render_server_dll_test ServerDll.cpp)

# Only the plugin links Server/Native. All Server calls cross its explicit exported API.
target_link_libraries(render_server_dll_test PRIVATE
    render_commands
    lux::engine::platform::platform_ipc
    render_resource
    lux::engine::core::core_execution
)

add_dependencies(render_server_dll_test render_server_dll_plugin)

set_target_properties(render_server_dll_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.server_dll COMMAND render_server_dll_test)

set_tests_properties(render.runtime.server_dll PROPERTIES TIMEOUT 45)

add_executable(render_storage_registration_test StorageRegistration.cpp)

target_link_libraries(render_storage_registration_test PRIVATE render_runtime)

set_target_properties(render_storage_registration_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.storage_registration COMMAND render_storage_registration_test)

set_tests_properties(render.runtime.storage_registration PROPERTIES TIMEOUT 30)

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

add_executable(render_runtime_offscreen_test Offscreen.cpp ../../native/vulkan/test/Allocation.cpp ${LUX_RUNTIME_SOURCES})
target_compile_definitions(render_runtime_offscreen_test PRIVATE LUX_RUNTIME_EFFECT_TEST_SEAM LUX_RUNTIME_ASYNC_TEST_SEAM)
target_include_directories(render_runtime_offscreen_test PRIVATE ../pinclude ../../native/vulkan/sinclude)
target_include_directories(render_runtime_offscreen_test PRIVATE ../../native/vulkan/test)
target_link_libraries(render_runtime_offscreen_test PRIVATE render_runtime)
set_target_properties(render_runtime_offscreen_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME render.runtime.offscreen COMMAND render_runtime_offscreen_test)
set_tests_properties(render.runtime.offscreen PROPERTIES TIMEOUT 90)

# Identical server source, linked to the existing compile-time Native fault seam.
# Only completion observation is held; actual GPU submission and final wait are real.
add_executable(render_runtime_shutdown_test Shutdown.cpp ${LUX_RUNTIME_SOURCES})
target_include_directories(render_runtime_shutdown_test PRIVATE ../pinclude ../../native/vulkan/test ../../native/vulkan/sinclude)
target_link_libraries(render_runtime_shutdown_test PRIVATE render_commands render_features
    render_native_instrumented lux::engine::platform::platform_ipc lux::engine::platform::platform_process
)
set_target_properties(render_runtime_shutdown_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME render.runtime.shutdown COMMAND render_runtime_shutdown_test)
set_tests_properties(render.runtime.shutdown PROPERTIES TIMEOUT 60)

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

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/VulkanServer.cpp" CONTENT
    "#include <lux/engine/render/runtime/VulkanServer.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
)
add_library(render_runtime_header_VulkanServer OBJECT "${CMAKE_CURRENT_BINARY_DIR}/VulkanServer.cpp")
target_link_libraries(render_runtime_header_VulkanServer PRIVATE render_runtime)

foreach(header RenderServer CommandBinding Commands Error)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT
        "#include <lux/engine/render/runtime/${header}.hpp>\n#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native leak\n#endif\nstatic_assert(__cplusplus >= 202002L);\n"
    )
    add_library(render_runtime_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")
    if(header STREQUAL "RenderServer" OR header STREQUAL "CommandBinding")
        # Server declaration stays neutral; only VulkanServer exposes the selected Native installation.
        target_link_libraries(render_runtime_header_${header} PRIVATE
            render_runtime
        )
    else()
        target_link_libraries(render_runtime_header_${header} PRIVATE render_commands render_features)
    endif()
endforeach()

add_executable(render_runtime_programs_test Programs.cpp)

target_include_directories(render_runtime_programs_test PRIVATE ../../native/vulkan/test)
target_link_libraries(render_runtime_programs_test PRIVATE render_runtime)
set_target_properties(render_runtime_programs_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME render.runtime.programs COMMAND render_runtime_programs_test)
set_tests_properties(render.runtime.programs PROPERTIES TIMEOUT 60)

add_executable(render_runtime_operations_test FeatureOperations.cpp ../../native/vulkan/test/Allocation.cpp)

target_include_directories(render_runtime_operations_test PRIVATE ../../native/vulkan/test ../pinclude)
target_link_libraries(render_runtime_operations_test PRIVATE render_runtime)
set_target_properties(render_runtime_operations_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME render.runtime.operations COMMAND render_runtime_operations_test)
set_tests_properties(render.runtime.operations PROPERTIES TIMEOUT 60)

# Real open GPU-ready Feature using the same Runtime and Native algorithms with observation/fault hooks.
add_executable(render_runtime_gpu_ready_test GpuReady.cpp ${LUX_RUNTIME_SOURCES}
    ../../native/vulkan/test/Allocation.cpp
)

target_compile_definitions(render_runtime_gpu_ready_test PRIVATE LUX_RUNTIME_TEST_SEAM LUX_RUNTIME_RESPONSE_COST_SEAM)

target_include_directories(render_runtime_gpu_ready_test PRIVATE ../pinclude ../../native/vulkan/test ../../native/vulkan/sinclude)

target_link_libraries(render_runtime_gpu_ready_test PRIVATE render_commands render_features
    render_native_instrumented lux::engine::platform::platform_ipc lux::engine::platform::platform_process
)

set_target_properties(render_runtime_gpu_ready_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.gpu_ready COMMAND render_runtime_gpu_ready_test)

set_tests_properties(render.runtime.gpu_ready PROPERTIES TIMEOUT 120)

add_test(NAME render.runtime.gpu_ready_transfer COMMAND render_runtime_gpu_ready_test dedicated)
add_test(NAME render.runtime.gpu_ready_same_family COMMAND render_runtime_gpu_ready_test same-family)
set_tests_properties(render.runtime.gpu_ready_transfer render.runtime.gpu_ready_same_family PROPERTIES TIMEOUT 120)

add_executable(render_texture_progress_test TextureProgress.cpp ${LUX_RUNTIME_SOURCES})

target_include_directories(render_texture_progress_test PRIVATE ../pinclude ../../native/vulkan/test ../../native/vulkan/sinclude)

target_link_libraries(render_texture_progress_test PRIVATE render_commands render_features
    render_native_instrumented lux::engine::platform::platform_ipc lux::engine::platform::platform_process
)

set_target_properties(render_texture_progress_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.texture_progress COMMAND render_texture_progress_test)

set_tests_properties(render.runtime.texture_progress PROPERTIES TIMEOUT 90)

add_executable(render_texture_operations_test TextureOperations.cpp)

target_include_directories(render_texture_operations_test PRIVATE ../../native/vulkan/test)

target_link_libraries(render_texture_operations_test PRIVATE render_runtime)

set_target_properties(render_texture_operations_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.texture_operations COMMAND render_texture_operations_test)

set_tests_properties(render.runtime.texture_operations PROPERTIES TIMEOUT 60)

add_executable(render_texture_rendering_test TextureRendering.cpp ../../native/vulkan/test/Allocation.cpp
    ${LUX_RUNTIME_SOURCES}
)

target_include_directories(render_texture_rendering_test PRIVATE ../../native/vulkan/test ../pinclude ../../native/vulkan/sinclude)

target_link_libraries(render_texture_rendering_test PRIVATE render_commands render_features
    render_native_instrumented lux::engine::platform::platform_ipc lux::engine::platform::platform_process
)

set_target_properties(render_texture_rendering_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.texture_rendering COMMAND render_texture_rendering_test)
add_test(NAME render.runtime.texture_rendering_alias COMMAND render_texture_rendering_test alias)

set_tests_properties(render.runtime.texture_rendering render.runtime.texture_rendering_alias PROPERTIES TIMEOUT 90)

add_executable(render_buffer_extension_test BufferExtension.cpp ../../native/vulkan/test/Allocation.cpp)

target_include_directories(render_buffer_extension_test PRIVATE ../../native/vulkan/test)

target_link_libraries(render_buffer_extension_test PRIVATE render_runtime)

set_target_properties(render_buffer_extension_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.buffer_extension COMMAND render_buffer_extension_test)

set_tests_properties(render.runtime.buffer_extension PROPERTIES TIMEOUT 60)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ExtensionHeader.cpp" CONTENT
    "#include \"${CMAKE_CURRENT_SOURCE_DIR}/../../native/vulkan/test/extension/Buffer.hpp\"\n#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native leak\n#endif\nstatic_assert(__cplusplus >= 202002L);\n"
)

add_library(render_extension_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/ExtensionHeader.cpp")

target_link_libraries(render_extension_header PRIVATE render_features)

add_executable(render_runtime_async_test Async.cpp ${LUX_RUNTIME_SOURCES})

target_include_directories(render_runtime_async_test PRIVATE ../pinclude ../../native/vulkan/sinclude)

target_compile_definitions(render_runtime_async_test PRIVATE LUX_RUNTIME_ASYNC_TEST_SEAM)

target_link_libraries(render_runtime_async_test PRIVATE render_commands render_features render_native_vulkan
    lux::engine::platform::platform_ipc render_runtime lux::engine::platform::platform_process
)

set_target_properties(render_runtime_async_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.async COMMAND render_runtime_async_test)

set_tests_properties(render.runtime.async PROPERTIES TIMEOUT 90)

# These are real Engine-owned test hosts, not dependencies of the Render module.
foreach(target render_runtime_test render_runtime_lifetime_test render_runtime_offscreen_test
    render_runtime_shutdown_test render_runtime_programs_test render_runtime_gpu_ready_test
    render_texture_progress_test render_texture_operations_test render_texture_rendering_test
    render_buffer_extension_test
)
    target_link_libraries(${target} PRIVATE lux::engine::core::core_execution)
endforeach()

foreach(target render_runtime_shutdown_test render_runtime_gpu_ready_test
    render_texture_progress_test render_texture_rendering_test
)
    target_link_libraries(${target} PRIVATE render_runtime)
endforeach()

add_subdirectory(async_tail)

add_executable(render_runtime_async_cost_test AsyncCost.cpp ${LUX_RUNTIME_SOURCES})

target_include_directories(render_runtime_async_cost_test PRIVATE ../pinclude ../../native/vulkan/sinclude)

target_compile_definitions(render_runtime_async_cost_test PRIVATE LUX_RUNTIME_ASYNC_COST_SEAM)

target_link_libraries(render_runtime_async_cost_test PRIVATE
    render_commands render_features render_native_vulkan lux::engine::platform::platform_ipc
    render_runtime lux::engine::core::core_execution lux::engine::platform::platform_process
)

set_target_properties(render_runtime_async_cost_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.async_cost COMMAND render_runtime_async_cost_test)

set_tests_properties(render.runtime.async_cost PROPERTIES TIMEOUT 120)

add_test(
    NAME render.runtime.client_installed
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/client_installed"
        "-DDEPENDENCY_DIR=${lux-cxx_DIR}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONFIG=$<CONFIG>"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../../core/test/Installed.cmake"
)

set_tests_properties(render.runtime.client_installed PROPERTIES TIMEOUT 120)

add_executable(render_feature_registration_test FeatureRegistration.cpp)

target_link_libraries(render_feature_registration_test PRIVATE render_runtime)

set_target_properties(render_feature_registration_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.features.operations COMMAND render_feature_registration_test)

set_tests_properties(render.features.operations PROPERTIES TIMEOUT 60)

add_executable(render_handler_ownership_test Handlers.cpp)

target_link_libraries(render_handler_ownership_test PRIVATE render_runtime)

set_target_properties(render_handler_ownership_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.handlers COMMAND render_handler_ownership_test)

set_tests_properties(render.runtime.handlers PROPERTIES TIMEOUT 60)

add_executable(render_mesh_operations_test MeshOperations.cpp)

target_link_libraries(render_mesh_operations_test PRIVATE render_runtime)

set_target_properties(render_mesh_operations_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.mesh_operations COMMAND render_mesh_operations_test)

set_tests_properties(render.runtime.mesh_operations PROPERTIES TIMEOUT 90)

add_executable(render_material_operations_test MaterialOperations.cpp)

target_link_libraries(render_material_operations_test PRIVATE render_runtime render_native_vulkan_extensions)

set_target_properties(render_material_operations_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_dependencies(render_material_operations_test render_material_assets)

add_test(NAME render.runtime.material_operations COMMAND render_material_operations_test
    "${CMAKE_BINARY_DIR}/render/native/vulkan/test/assets/Unlit.frag.spv"
)

set_tests_properties(render.runtime.material_operations PROPERTIES TIMEOUT 90)

add_executable(render_instance_operations_test InstanceOperations.cpp)

target_link_libraries(render_instance_operations_test PRIVATE render_runtime render_native_vulkan_extensions)

set_target_properties(render_instance_operations_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_dependencies(render_instance_operations_test render_material_assets)

add_test(NAME render.runtime.instance_operations COMMAND render_instance_operations_test
    "${CMAKE_BINARY_DIR}/render/native/vulkan/test/assets/Unlit.frag.spv"
)

set_tests_properties(render.runtime.instance_operations PROPERTIES TIMEOUT 90)

add_executable(render_camera_test Camera.cpp)

target_link_libraries(render_camera_test PRIVATE render_runtime render_native_vulkan_extensions)

set_target_properties(render_camera_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.camera COMMAND render_camera_test)

set_tests_properties(render.runtime.camera PROPERTIES TIMEOUT 60)

add_executable(render_feature_bundle_test FeatureBundle.cpp)

target_link_libraries(render_feature_bundle_test PRIVATE render_runtime render_native_vulkan_extensions)

set_target_properties(render_feature_bundle_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.feature_bundle COMMAND render_feature_bundle_test)

set_tests_properties(render.runtime.feature_bundle PROPERTIES TIMEOUT 60)

add_subdirectory(basic_mesh)

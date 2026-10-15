add_executable(render_runtime_test Runtime.cpp)

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

add_executable(render_runtime_offscreen_test Offscreen.cpp ../../native/vulkan/test/Allocation.cpp)
target_include_directories(render_runtime_offscreen_test PRIVATE ../../native/vulkan/test)
target_link_libraries(render_runtime_offscreen_test PRIVATE render_runtime)
set_target_properties(render_runtime_offscreen_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME render.runtime.offscreen COMMAND render_runtime_offscreen_test)
set_tests_properties(render.runtime.offscreen PROPERTIES TIMEOUT 90)

# Identical server source, linked to the existing compile-time Native fault seam.
# Only completion observation is held; actual GPU submission and final wait are real.
add_executable(render_runtime_shutdown_test Shutdown.cpp ../src/RenderServer.cpp ../src/Request.cpp)
target_include_directories(render_runtime_shutdown_test PRIVATE ../pinclude ../../native/vulkan/test)
target_link_libraries(render_runtime_shutdown_test PRIVATE render_client render_features
    render_native_instrumented lux::engine::platform::platform_ipc bcrypt
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

foreach(header RenderClient RenderServer Operations)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT
        "#include <lux/engine/render/runtime/${header}.hpp>\n#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native leak\n#endif\nstatic_assert(__cplusplus >= 202002L);\n"
    )
    add_library(render_runtime_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")
    # Cooked Program bytes have no Vulkan/OS dependency; do not link Native for this header probe.
    target_link_libraries(render_runtime_header_${header} PRIVATE render_client render_features)
endforeach()

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GpuCompletion.cpp" CONTENT
    "#include <lux/engine/render/runtime/vulkan/GpuCompletion.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
)

add_library(render_runtime_header_GpuCompletion OBJECT "${CMAKE_CURRENT_BINARY_DIR}/GpuCompletion.cpp")

target_link_libraries(render_runtime_header_GpuCompletion PRIVATE render_runtime)

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
add_executable(render_runtime_gpu_ready_test GpuReady.cpp ../src/RenderServer.cpp ../src/Request.cpp
    ../../native/vulkan/test/Allocation.cpp
)

target_compile_definitions(render_runtime_gpu_ready_test PRIVATE LUX_RUNTIME_TEST_SEAM)

target_include_directories(render_runtime_gpu_ready_test PRIVATE ../pinclude ../../native/vulkan/test)

target_link_libraries(render_runtime_gpu_ready_test PRIVATE render_client render_features
    render_native_instrumented lux::engine::platform::platform_ipc bcrypt
)

set_target_properties(render_runtime_gpu_ready_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.gpu_ready COMMAND render_runtime_gpu_ready_test)

set_tests_properties(render.runtime.gpu_ready PROPERTIES TIMEOUT 120)

add_test(NAME render.runtime.gpu_ready_transfer COMMAND render_runtime_gpu_ready_test dedicated)
add_test(NAME render.runtime.gpu_ready_same_family COMMAND render_runtime_gpu_ready_test same-family)
set_tests_properties(render.runtime.gpu_ready_transfer render.runtime.gpu_ready_same_family PROPERTIES TIMEOUT 120)

add_executable(render_texture_progress_test TextureProgress.cpp ../src/RenderServer.cpp ../src/Request.cpp)

target_include_directories(render_texture_progress_test PRIVATE ../pinclude ../../native/vulkan/test)

target_link_libraries(render_texture_progress_test PRIVATE render_client render_features
    render_native_instrumented lux::engine::platform::platform_ipc bcrypt
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
    ../src/RenderServer.cpp ../src/Request.cpp ../src/TextureStorage.cpp
)

target_include_directories(render_texture_rendering_test PRIVATE ../../native/vulkan/test ../pinclude)

target_link_libraries(render_texture_rendering_test PRIVATE render_client render_features
    render_native_instrumented lux::engine::platform::platform_ipc bcrypt
)

set_target_properties(render_texture_rendering_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.runtime.texture_rendering COMMAND render_texture_rendering_test)
add_test(NAME render.runtime.texture_rendering_alias COMMAND render_texture_rendering_test alias)

set_tests_properties(render.runtime.texture_rendering render.runtime.texture_rendering_alias PROPERTIES TIMEOUT 90)

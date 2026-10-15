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
    target_include_directories(render_runtime_header_${header} PRIVATE ../../native/vulkan/include)
    target_link_libraries(render_runtime_header_${header} PRIVATE render_client render_features)
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

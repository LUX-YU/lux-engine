add_executable(render_vulkan_device Device.cpp)

target_link_libraries(render_vulkan_device PRIVATE lux::engine::render::render_vulkan)

add_test(NAME render.vulkan.device COMMAND render_vulkan_device)

set_tests_properties(render.vulkan.device PROPERTIES RUN_SERIAL TRUE)

if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR "R4 qualification requires the installed Vulkan glslc tool")
endif()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Smoke.spv"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.3
        "${CMAKE_CURRENT_SOURCE_DIR}/Smoke.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/Smoke.spv"
    DEPENDS Smoke.comp
    VERBATIM
)

add_custom_target(render_vulkan_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Smoke.spv")

add_executable(render_vulkan_objects Objects.cpp)

target_link_libraries(render_vulkan_objects PRIVATE lux::engine::render::render_vulkan)

add_dependencies(render_vulkan_objects render_vulkan_shader)

add_test(NAME render.vulkan.objects COMMAND render_vulkan_objects "${CMAKE_CURRENT_BINARY_DIR}/Smoke.spv")

set_tests_properties(render.vulkan.objects PROPERTIES RUN_SERIAL TRUE)

add_executable(render_vulkan_transfer Transfer.cpp)

target_link_libraries(render_vulkan_transfer PRIVATE lux::engine::render::render_vulkan)

add_dependencies(render_vulkan_transfer render_vulkan_shader)

add_test(NAME render.vulkan.transfer COMMAND render_vulkan_transfer "${CMAKE_CURRENT_BINARY_DIR}/Smoke.spv")

set_tests_properties(render.vulkan.transfer PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)

# Compile the SAME native implementation with a private call-site seam. No seam
# symbols or function tables occur in render_vulkan or its performance consumer.
list(TRANSFORM LUX_VULKAN_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/../" OUTPUT_VARIABLE fault_sources)

add_library(render_vulkan_fault STATIC ${fault_sources})

target_include_directories(render_vulkan_fault PUBLIC ../include PRIVATE ../pinclude)

target_compile_definitions(render_vulkan_fault PRIVATE LUX_VULKAN_TEST_SEAM)

target_link_libraries(
    render_vulkan_fault
    PUBLIC lux::engine::render::render_core Vulkan::Vulkan
    PRIVATE GPUOpen::VulkanMemoryAllocator
)

add_executable(render_vulkan_faults Faults.cpp)

target_link_libraries(render_vulkan_faults PRIVATE render_vulkan_fault)

add_dependencies(render_vulkan_faults render_vulkan_shader)

add_test(NAME render.vulkan.faults COMMAND render_vulkan_faults "${CMAKE_CURRENT_BINARY_DIR}/Smoke.spv")

set_tests_properties(render.vulkan.faults PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_test(
    NAME render.vulkan.lifetime_contract
    COMMAND "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:render_vulkan_faults>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/Lifetime.cmake"
)

set_tests_properties(render.vulkan.lifetime_contract PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)

option(LUX_R4_ASAN "Instrument R4 native owners and ownership tests with AddressSanitizer" OFF)

add_executable(render_vulkan_benchmark Benchmark.cpp)

target_link_libraries(render_vulkan_benchmark PRIVATE lux::engine::render::render_vulkan)

if(NOT LUX_R4_ASAN)
    add_test(NAME render.vulkan.bounded_allocation COMMAND render_vulkan_benchmark)
    set_tests_properties(render.vulkan.bounded_allocation PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
endif()

foreach(header IN ITEMS Error device/Device memory/Memory descriptor/Descriptors pipeline/Pipeline
        transfer/Submission transfer/Transfer retirement/Retirement)
    string(REPLACE "/" "_" name "${header}")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${name}.cpp")
    file(WRITE "${source}" "#include <lux/engine/render/vulkan/${header}.hpp>\n")
    add_library(render_vulkan_header_${name} OBJECT "${source}")
    target_link_libraries(render_vulkan_header_${name} PRIVATE lux::engine::render::render_vulkan)
endforeach()

if(LUX_R4_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "R4 ASan qualification currently defines the MSVC configuration only")
    endif()

    foreach(target render_vulkan render_vulkan_fault render_vulkan_device render_vulkan_objects
            render_vulkan_transfer render_vulkan_faults render_vulkan_benchmark)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    endforeach()
endif()

add_executable(render_vulkan_probe_control Probe.cpp)

target_link_libraries(render_vulkan_probe_control PRIVATE lux::engine::render::render_vulkan)

add_test(NAME render.vulkan.probe_control COMMAND render_vulkan_probe_control)

set(rejected_headers
    lux/engine/scene/SceneSystem.hpp
    lux/engine/render/runtime/RenderRuntime.hpp
    lux/engine/render/transport/Transport.hpp
    lux/engine/render/graph/Bindings.hpp
    lux/engine/function/render/graph/DependencyAnalyzer.hpp
    lux/engine/editor/LuxEngine.hpp
    lux/engine/render/features/Feature.hpp
)

set(probes SCENE RUNTIME TRANSPORT GRAPH LEGACY EDITOR FEATURE)

foreach(index RANGE 0 6)
    list(GET rejected_headers ${index} header)
    list(GET probes ${index} probe)
    add_executable(render_vulkan_reject_${probe} EXCLUDE_FROM_ALL Probe.cpp)
    target_compile_definitions(render_vulkan_reject_${probe} PRIVATE "LUX_FORBIDDEN_INCLUDE=\"${header}\"")
    target_link_libraries(render_vulkan_reject_${probe} PRIVATE lux::engine::render::render_vulkan)
    add_test(
        NAME render.vulkan.reject_${probe}
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DPROBE=${probe}" "-DHEADER=${header}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/RejectCompile.cmake"
    )
    set_tests_properties(render.vulkan.reject_${probe} PROPERTIES RUN_SERIAL TRUE)
endforeach()

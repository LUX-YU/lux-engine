add_executable(render_vulkan_device Device.cpp)

add_subdirectory(shader)

add_executable(render_vulkan_shader_benchmark ShaderBenchmark.cpp)

target_link_libraries(render_vulkan_shader_benchmark PRIVATE render_vulkan_shader_compiler)

target_include_directories(
    render_vulkan_shader_benchmark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

add_dependencies(
    render_vulkan_shader_benchmark render_vulkan_spv_F3SharedA_vert render_vulkan_spv_F3SharedA_frag
        render_vulkan_spv_F3SharedB_vert render_vulkan_spv_F3SharedB_frag
)

add_test(
    NAME render.vulkan.shader_benchmark
    COMMAND render_vulkan_shader_benchmark "${CMAKE_CURRENT_BINARY_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/shader"
        "${CMAKE_CURRENT_BINARY_DIR}/benchmark-evidence"
)

set_tests_properties(render.vulkan.shader_benchmark PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)

add_executable(render_vulkan_shader_hot_link ShaderHotLink.cpp)

target_link_libraries(render_vulkan_shader_hot_link PRIVATE render_vulkan_shader_compiler)

if(MSVC)
    target_link_options(render_vulkan_shader_hot_link PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/shader-hot.map")
endif()

add_test(NAME render.vulkan.shader_hot_link COMMAND render_vulkan_shader_hot_link)

add_executable(render_vulkan_shader_cold ShaderColdFailures.cpp)

target_link_libraries(render_vulkan_shader_cold PRIVATE render_vulkan_shader_compiler)

target_include_directories(
    render_vulkan_shader_cold PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

add_dependencies(
    render_vulkan_shader_cold render_vulkan_spv_F3Storage_comp render_vulkan_spv_F3SharedA_vert render_vulkan_spv_F3SharedA_frag
)

add_test(NAME render.vulkan.shader_cold COMMAND render_vulkan_shader_cold "${CMAKE_CURRENT_BINARY_DIR}/shader")

set_tests_properties(render.vulkan.shader_cold PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_vulkan_raster_state RasterState.cpp)

target_link_libraries(render_vulkan_raster_state PRIVATE render_vulkan_shader_compiler)

target_include_directories(
    render_vulkan_raster_state PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

add_dependencies(render_vulkan_raster_state render_vulkan_spv_F3Raster_vert render_vulkan_spv_F3Raster_frag)

add_test(
    NAME render.vulkan.raster_state
    COMMAND render_vulkan_raster_state "${CMAKE_CURRENT_BINARY_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/shader"
        "${CMAKE_CURRENT_BINARY_DIR}/raster-evidence"
)

set_tests_properties(render.vulkan.raster_state PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_vulkan_shared_owners SharedOwners.cpp)

target_link_libraries(render_vulkan_shared_owners PRIVATE render_vulkan_shader_compiler)

target_include_directories(
    render_vulkan_shared_owners PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

add_dependencies(
    render_vulkan_shared_owners render_vulkan_spv_F3SharedA_vert render_vulkan_spv_F3SharedA_frag
        render_vulkan_spv_F3SharedB_vert render_vulkan_spv_F3SharedB_frag
)

add_test(
    NAME render.vulkan.shared_owners
    COMMAND render_vulkan_shared_owners "${CMAKE_CURRENT_BINARY_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/shader"
        "${CMAKE_CURRENT_BINARY_DIR}/shared-evidence"
)

set_tests_properties(render.vulkan.shared_owners PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_vulkan_compute_bindings ComputeBindings.cpp)

target_link_libraries(render_vulkan_compute_bindings PRIVATE render_vulkan_shader_compiler)

target_include_directories(
    render_vulkan_compute_bindings PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

add_dependencies(render_vulkan_compute_bindings render_vulkan_spv_F3Storage_comp)

add_test(
    NAME render.vulkan.compute_bindings
    COMMAND render_vulkan_compute_bindings "${CMAKE_CURRENT_BINARY_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/shader"
        "${CMAKE_CURRENT_BINARY_DIR}/compute-evidence"
)

set_tests_properties(render.vulkan.compute_bindings PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_vulkan_attachment_clear AttachmentClear.cpp)

target_link_libraries(render_vulkan_attachment_clear PRIVATE render_vulkan_shader_compiler)

target_include_directories(
    render_vulkan_attachment_clear PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

add_dependencies(render_vulkan_attachment_clear render_vulkan_schema_F3Tonemap)

add_test(
    NAME render.vulkan.attachment_clear
    COMMAND render_vulkan_attachment_clear "${CMAKE_CURRENT_BINARY_DIR}/clear-evidence"
)

set_tests_properties(render.vulkan.attachment_clear PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_vulkan_postprocess Postprocess.cpp)

target_link_libraries(render_vulkan_postprocess PRIVATE render_vulkan_shader_compiler)

target_include_directories(
    render_vulkan_postprocess PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

add_dependencies(
    render_vulkan_postprocess render_vulkan_spv_F3Tonemap_vert render_vulkan_spv_F3Tonemap_frag
        render_vulkan_spv_F3Blur_vert render_vulkan_spv_F3Blur_frag
        render_vulkan_spv_F3Composite_vert render_vulkan_spv_F3Composite_frag render_vulkan_spv_additive
)

add_test(
    NAME render.vulkan.postprocess_w01
    COMMAND render_vulkan_postprocess "${CMAKE_CURRENT_BINARY_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/shader"
        "${CMAKE_CURRENT_BINARY_DIR}/w01-evidence"
)

set_tests_properties(render.vulkan.postprocess_w01 PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_vulkan_shader_assets ShaderAssets.cpp)

target_link_libraries(render_vulkan_shader_assets PRIVATE render_vulkan render_graph toolchain_shader_reflection)

target_include_directories(
    render_vulkan_shader_assets PRIVATE "${CMAKE_BINARY_DIR}/render_graph/test/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../graph/test/authoring"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine/toolchain/shader/include"
)

add_dependencies(render_vulkan_shader_assets render_graph_shader_Complex)

add_test(
    NAME render.vulkan.shader_assets
    COMMAND render_vulkan_shader_assets "${CMAKE_BINARY_DIR}/render_graph/test/generated/Complex.spv"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../graph/test/authoring/Complex.lglsl"
)

add_executable(render_vulkan_shader_layout ShaderLayout.cpp)

target_link_libraries(render_vulkan_shader_layout PRIVATE render_vulkan_shader_compiler render_graph toolchain_shader_reflection)

target_include_directories(
    render_vulkan_shader_layout PRIVATE "${CMAKE_BINARY_DIR}/render_graph/test/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../graph/test/authoring"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine/toolchain/shader/include"
)

add_dependencies(render_vulkan_shader_layout render_graph_shader_Complex render_graph_shader_Stages.vert render_graph_shader_Stages.frag)

add_test(
    NAME render.vulkan.shader_layout
    COMMAND render_vulkan_shader_layout
        "${CMAKE_BINARY_DIR}/render_graph/test/generated/Complex.spv"
        "${CMAKE_BINARY_DIR}/render_graph/test/generated/Stages.vert.spv"
        "${CMAKE_BINARY_DIR}/render_graph/test/generated/Stages.frag.spv"
)

set_tests_properties(render.vulkan.shader_layout PROPERTIES RUN_SERIAL TRUE)

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

add_library(
    render_vulkan_shader_fault STATIC ../src/shader/Layout.cpp ../src/shader/Program.cpp ../src/shader/Bindings.cpp
)

target_include_directories(
    render_vulkan_shader_fault PUBLIC ../include "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine/toolchain/shader/include"
)

target_link_libraries(
    render_vulkan_shader_fault PUBLIC render_vulkan_fault shader_metadata render_graph PRIVATE toolchain_shader_reflection
)

add_executable(render_vulkan_shader_candidate ShaderCandidate.cpp)

target_link_libraries(render_vulkan_shader_candidate PRIVATE render_vulkan_shader_fault)

target_include_directories(
    render_vulkan_shader_candidate PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/type_info/include"
)

add_dependencies(
    render_vulkan_shader_candidate render_vulkan_spv_F3SharedA_vert render_vulkan_spv_F3SharedA_frag
        render_vulkan_spv_F3SharedB_vert render_vulkan_spv_F3SharedB_frag render_vulkan_spv_F3Raster_vert render_vulkan_spv_F3Raster_frag
)

add_test(
    NAME render.vulkan.shader_candidate
    COMMAND render_vulkan_shader_candidate "${CMAKE_CURRENT_BINARY_DIR}/shader" "${CMAKE_CURRENT_SOURCE_DIR}/shader"
        "${CMAKE_CURRENT_BINARY_DIR}/candidate-evidence"
)

set_tests_properties(render.vulkan.shader_candidate PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

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

foreach(header IN ITEMS Error device/Device memory/Memory descriptor/Descriptors descriptor/ImageBindings pipeline/Pipeline pipeline/Graphics
        transfer/Submission transfer/Transfer retirement/Retirement)
    string(REPLACE "/" "_" name "${header}")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${name}.cpp")
    file(WRITE "${source}" "#include <lux/engine/render/vulkan/${header}.hpp>\n")
    add_library(render_vulkan_header_${name} OBJECT "${source}")
    target_link_libraries(render_vulkan_header_${name} PRIVATE lux::engine::render::render_vulkan)
endforeach()

foreach(header IN ITEMS Layout Format Program Bindings)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_shader_${header}.cpp")
    file(WRITE "${source}" "#include <lux/engine/render/vulkan/shader/${header}.hpp>\n")
    add_library(render_vulkan_header_shader_${header} OBJECT "${source}")
    target_link_libraries(render_vulkan_header_shader_${header} PRIVATE render_vulkan_shader_compiler)
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

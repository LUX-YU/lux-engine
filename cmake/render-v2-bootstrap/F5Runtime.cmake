set(f5_test "${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render/runtime/test")
set(f5_native_test "${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render/vulkan/test")
set(f5_shader "${CMAKE_BINARY_DIR}/render_vulkan/test/shader")

add_executable(render_runtime_native_invocations
    "${f5_test}/NativeInvocations.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_runtime_native_invocations PRIVATE render_vulkan_shader_compiler)

target_include_directories(render_runtime_native_invocations PRIVATE
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_runtime_native_invocations
    render_vulkan_spv_F4Fill_comp render_vulkan_spv_F3Tonemap_vert render_vulkan_spv_F3Tonemap_frag
    render_vulkan_schema_F4Readback
)

add_test(NAME render.runtime.native_invocations COMMAND render_runtime_native_invocations "${f5_shader}")

set_tests_properties(render.runtime.native_invocations PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_runtime_target_extent
    "${f5_test}/TargetExtent.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_runtime_target_extent PRIVATE render_vulkan_shader_compiler)

target_include_directories(render_runtime_target_extent PRIVATE
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_runtime_target_extent
    render_vulkan_spv_F4Fill_comp render_vulkan_spv_F3Tonemap_vert render_vulkan_spv_F3Tonemap_frag
    render_vulkan_schema_F4HostRead
)

add_test(NAME render.runtime.target_extent COMMAND render_runtime_target_extent "${f5_shader}")

set_tests_properties(render.runtime.target_extent PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_runtime_progress
    "${f5_test}/RuntimeProgress.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_runtime_progress PRIVATE render_runtime render_vulkan_shader_compiler)

target_include_directories(render_runtime_progress PRIVATE
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_runtime_progress
    render_vulkan_spv_F4Fill_comp render_vulkan_spv_F3Tonemap_vert render_vulkan_spv_F3Tonemap_frag
    render_vulkan_schema_F4HostRead
)

add_test(NAME render.runtime.progress COMMAND render_runtime_progress "${f5_shader}")

set_tests_properties(render.runtime.progress PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_runtime_schema_F5Shared "${f5_native_test}/../../graph/test/authoring/Fixture.cpp")

target_link_libraries(render_runtime_schema_F5Shared PRIVATE
    render_graph toolchain_shader_emitter toolchain_shader_reflection
)

target_include_directories(render_runtime_schema_F5Shared PRIVATE
    "${f5_test}/shader" "${f5_native_test}/../../../../core/type_info/include"
)

target_compile_definitions(render_runtime_schema_F5Shared PRIVATE
    "LUX_FIXTURE_HEADER=\"F5Shared.pass.hpp\"" "LUX_FIXTURE_TYPE=F5Shared"
)

render_pass_schema(render_runtime_schema_F5Shared "${f5_test}/shader/F5Shared.hpp")

add_custom_command(
    OUTPUT "${f5_shader}/F5Shared.comp.glsl" "${f5_shader}/F5Shared.comp.spv"
    COMMAND render_runtime_schema_F5Shared "${f5_test}/shader/F5Shared.comp.lglsl" "${f5_shader}/F5Shared.comp.glsl"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -fshader-stage=comp --target-env=vulkan1.3
        "${f5_shader}/F5Shared.comp.glsl" -o "${f5_shader}/F5Shared.comp.spv"
    COMMAND "${LUX_SPIRV_VAL}" --target-env vulkan1.3 "${f5_shader}/F5Shared.comp.spv"
    COMMAND render_runtime_schema_F5Shared "${f5_shader}/F5Shared.comp.spv" validate
    DEPENDS render_runtime_schema_F5Shared "${f5_test}/shader/F5Shared.comp.lglsl"
    VERBATIM
)

add_custom_target(render_runtime_spv_F5Shared ALL DEPENDS "${f5_shader}/F5Shared.comp.spv")

add_executable(render_runtime_shared_asset
    "${f5_test}/SharedAsset.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_runtime_shared_asset PRIVATE render_runtime render_vulkan_shader_compiler)

target_include_directories(render_runtime_shared_asset PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${f5_test}/shader"
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_runtime_shared_asset render_runtime_spv_F5Shared render_vulkan_schema_F4HostRead
    render_vulkan_spv_F3Tonemap_vert render_vulkan_spv_F3Tonemap_frag
)

add_test(NAME render.runtime.shared_asset COMMAND render_runtime_shared_asset "${f5_shader}")

set_tests_properties(render.runtime.shared_asset PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_runtime_presentation
    "${f5_test}/Presentation.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_runtime_presentation PRIVATE render_runtime render_vulkan_shader_compiler user32)

target_include_directories(render_runtime_presentation PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${f5_test}/shader"
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_runtime_presentation render_runtime_spv_F5Shared render_vulkan_schema_F4HostRead)

add_test(NAME render.runtime.presentation COMMAND render_runtime_presentation "${f5_shader}")

set_tests_properties(render.runtime.presentation PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

# The same Runtime implementation, with only the already existing native
# Foundation fault seam beneath it. No test controls enter Runtime's public ABI.
add_library(render_runtime_fault STATIC
    "${f5_test}/../src/Backend.cpp" "${f5_test}/../src/Frame.cpp"
    "${f5_test}/../src/Runtime.cpp" "${f5_test}/../src/Presentation.cpp"
)

target_include_directories(render_runtime_fault PUBLIC
    "${f5_test}/../include" "${CMAKE_BINARY_DIR}/render_runtime/generated"
    "${f5_native_test}/../../../../core/type_info/include"
    PRIVATE "${f5_test}/../pinclude"
)

target_compile_definitions(render_runtime_fault PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(render_runtime_fault PUBLIC render_core render_transport render_graph
    PRIVATE render_vulkan_graph_fault
)

add_dependencies(render_runtime_fault render_runtime)

add_executable(render_runtime_lifecycle
    "${f5_test}/Lifecycle.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_runtime_lifecycle PRIVATE render_runtime_fault render_vulkan_graph_fault)

target_include_directories(render_runtime_lifecycle PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${f5_test}/shader"
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_runtime_lifecycle render_runtime_spv_F5Shared render_vulkan_schema_F4HostRead)

add_test(NAME render.runtime.lifecycle COMMAND render_runtime_lifecycle "${f5_shader}")

set_tests_properties(render.runtime.lifecycle PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
add_executable(render_runtime_schema_F5History "${f5_native_test}/../../graph/test/authoring/Fixture.cpp")

target_link_libraries(render_runtime_schema_F5History PRIVATE
    render_graph toolchain_shader_emitter toolchain_shader_reflection
)

target_include_directories(render_runtime_schema_F5History PRIVATE
    "${f5_test}/shader" "${f5_native_test}/../../../../core/type_info/include"
)

target_compile_definitions(render_runtime_schema_F5History PRIVATE
    "LUX_FIXTURE_HEADER=\"F5History.pass.hpp\"" "LUX_FIXTURE_TYPE=F5History"
)

render_pass_schema(render_runtime_schema_F5History "${f5_test}/shader/F5History.hpp")

add_custom_command(
    OUTPUT "${f5_shader}/F5History.comp.glsl" "${f5_shader}/F5History.comp.spv"
    COMMAND render_runtime_schema_F5History "${f5_test}/shader/F5History.comp.lglsl" "${f5_shader}/F5History.comp.glsl"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -fshader-stage=comp --target-env=vulkan1.3
        "${f5_shader}/F5History.comp.glsl" -o "${f5_shader}/F5History.comp.spv"
    COMMAND "${LUX_SPIRV_VAL}" --target-env vulkan1.3 "${f5_shader}/F5History.comp.spv"
    COMMAND render_runtime_schema_F5History "${f5_shader}/F5History.comp.spv" validate
    DEPENDS render_runtime_schema_F5History "${f5_test}/shader/F5History.comp.lglsl"
    VERBATIM
)

add_custom_target(render_runtime_spv_F5History ALL DEPENDS "${f5_shader}/F5History.comp.spv")


add_executable(render_runtime_history
    "${f5_test}/History.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_runtime_history PRIVATE render_runtime render_vulkan_shader_compiler)

target_include_directories(render_runtime_history PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${f5_test}/shader"
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_runtime_history render_runtime_spv_F5History render_vulkan_schema_F4HostRead)

add_test(NAME render.runtime.history COMMAND render_runtime_history "${f5_shader}")

set_tests_properties(render.runtime.history PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

# Each public header is parsed as the first include in a distinct translation unit.
foreach(header Types.hpp RuntimeMessages.hpp RuntimeMessages.ops.hpp Runtime.hpp NativeAssembly.hpp)
    string(REPLACE "." "_" header_target "${header}")
    set(header_source "${CMAKE_CURRENT_BINARY_DIR}/f5_header_${header_target}.cpp")
    file(WRITE "${header_source}" "#include <lux/engine/render/runtime/${header}>\n")
    add_library(render_runtime_header_${header_target} OBJECT "${header_source}")
    target_link_libraries(render_runtime_header_${header_target} PRIVATE render_runtime)
    if(header STREQUAL "NativeAssembly.hpp")
        target_link_libraries(render_runtime_header_${header_target} PRIVATE render_vulkan_graph)
    else()
        file(APPEND "${header_source}" "#ifdef VK_VERSION_1_0\n#error Neutral Runtime header leaked Vulkan\n#endif\n")
    endif()
endforeach()

add_executable(render_runtime_probe_control "${f5_test}/Probe.cpp")

target_link_libraries(render_runtime_probe_control PRIVATE render_runtime)

add_test(NAME render.runtime.public_contract COMMAND render_runtime_probe_control)

foreach(probe ID REQUEST_COPY)
    add_executable(render_runtime_reject_${probe} EXCLUDE_FROM_ALL "${f5_test}/Probe.cpp")
    target_link_libraries(render_runtime_reject_${probe} PRIVATE render_runtime)
    target_compile_definitions(render_runtime_reject_${probe} PRIVATE LUX_PROBE_${probe})
    add_test(NAME render.runtime.reject_${probe}
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DPROBE=${probe}"
            -P "${f5_test}/RejectCompile.cmake"
    )
    set_tests_properties(render.runtime.reject_${probe} PROPERTIES RUN_SERIAL TRUE)
endforeach()

find_package(VulkanMemoryAllocator 3 CONFIG REQUIRED)

# Reuse the exact Native graph source list, replacing only the preparation TU
# with a source-inclusion timing wrapper. The production target is untouched.
get_target_property(f5_native_sources render_vulkan_graph SOURCES)
get_target_property(f5_native_source_dir render_vulkan_graph SOURCE_DIR)
set(f5_profile_sources)
foreach(source IN LISTS f5_native_sources)
    if(source STREQUAL "src/graph/Frame.cpp")
        list(APPEND f5_profile_sources "${f5_test}/ProfileNativeFrame.cpp")
    else()
        list(APPEND f5_profile_sources "${f5_native_source_dir}/${source}")
    endif()
endforeach()

add_library(render_vulkan_graph_profile STATIC ${f5_profile_sources})

target_include_directories(render_vulkan_graph_profile PUBLIC
    "${f5_native_source_dir}/include" "${f5_native_source_dir}/../../../../engine/toolchain/shader/include"
    PRIVATE "${f5_native_source_dir}/pinclude"
)

target_link_libraries(render_vulkan_graph_profile PUBLIC render_vulkan shader_metadata render_graph)

# Profiling uses an exact source inclusion with a test-only free-function wrapper.
# Neither timestamps nor instrumentation are linked into render_runtime.
add_library(render_runtime_profile STATIC
    "${f5_test}/../src/Backend.cpp" "${f5_test}/ProfileFrame.cpp"
    "${f5_test}/../src/Runtime.cpp" "${f5_test}/../src/Presentation.cpp"
)

target_include_directories(render_runtime_profile PUBLIC
    "${f5_test}/../include" "${CMAKE_BINARY_DIR}/render_runtime/generated"
    "${f5_native_test}/../../../../core/type_info/include"
    PRIVATE "${f5_test}/../pinclude"
)

target_compile_definitions(render_runtime_profile PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(render_runtime_profile PUBLIC render_core render_transport render_graph
    PRIVATE render_vulkan_graph_profile GPUOpen::VulkanMemoryAllocator
)

add_dependencies(render_runtime_profile render_runtime)

add_executable(render_runtime_benchmark
    "${f5_test}/RuntimeBenchmark.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_runtime_benchmark PRIVATE render_runtime_profile render_vulkan_shader_compiler)

target_link_options(render_runtime_benchmark PRIVATE "/MAP:${CMAKE_BINARY_DIR}/runtime-profile.map")

target_include_directories(render_runtime_benchmark PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${f5_test}/shader"
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_runtime_benchmark render_runtime_spv_F5Shared render_vulkan_schema_F4HostRead)

add_executable(render_runtime_hot_link "${f5_test}/HotLink.cpp")

target_link_libraries(render_runtime_hot_link PRIVATE render_runtime render_vulkan_graph)

target_link_options(render_runtime_hot_link PRIVATE "/MAP:${CMAKE_BINARY_DIR}/runtime-hot.map")

add_test(NAME render.runtime.shared_asset_partial_range
    COMMAND render_runtime_shared_asset "${f5_shader}" partial-range
)

set_tests_properties(render.runtime.shared_asset_partial_range PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_test(NAME render.runtime.shared_asset_partition_refinement
    COMMAND render_runtime_shared_asset "${f5_shader}" partial-range different-partition
)

set_tests_properties(render.runtime.shared_asset_partition_refinement PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

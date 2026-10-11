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

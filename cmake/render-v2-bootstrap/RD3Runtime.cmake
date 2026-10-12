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

add_executable(render_runtime_import_scale "${f5_test}/ImportScale.cpp")

target_link_libraries(render_runtime_import_scale PRIVATE render_vulkan_graph_profile)

target_include_directories(render_runtime_import_scale PRIVATE
    "${f5_test}/shader" "${f5_native_test}" "${f5_native_test}/../../../../core/type_info/include"
)

render_pass_schema(render_runtime_import_scale "${f5_test}/shader/F5CReadBuffer.hpp")

add_test(NAME render.runtime.import_scale COMMAND render_runtime_import_scale)

set_tests_properties(render.runtime.import_scale PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)

add_executable(render_rd3_runtime
    "${f5_test}/RD3Runtime.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_rd3_runtime PRIVATE render_runtime render_vulkan_shader_compiler)

target_include_directories(render_rd3_runtime PRIVATE
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_rd3_runtime
    render_vulkan_spv_F4Fill_comp render_vulkan_spv_F3Tonemap_vert render_vulkan_spv_F3Tonemap_frag
    render_vulkan_schema_F4HostRead
)

add_test(NAME render.rd3.runtime COMMAND render_rd3_runtime "${f5_shader}")

set_tests_properties(render.rd3.runtime PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_executable(render_rd3_faults
    "${f5_test}/RD3Faults.cpp" "${f5_test}/../src/Runtime.cpp" "${f5_test}/../src/DeviceSession.cpp" "${f5_native_test}/NativeGraphSupport.cpp"
)

target_link_libraries(render_rd3_faults PRIVATE render_vulkan_graph_fault render_transport)

target_compile_definitions(render_rd3_faults PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_include_directories(render_rd3_faults PRIVATE
    "${f5_test}/../include" "${f5_test}/../pinclude" "${CMAKE_BINARY_DIR}/render_runtime/generated"
    "${f5_native_test}" "${f5_native_test}/shader" "${f5_shader}/generated"
    "${f5_native_test}/../../../../core/type_info/include"
)

add_dependencies(render_rd3_faults render_rd3_runtime)

add_test(NAME render.rd3.faults COMMAND render_rd3_faults "${f5_shader}")

set_tests_properties(render.rd3.faults PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

foreach(header Objects Types Runtime Backend ResourceDomain NativeAssembly DeviceSession)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rd3_header_${header}.cpp"
        "#include <lux/engine/render/runtime/${header}.hpp>\n"
    )

    add_library(render_rd3_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/rd3_header_${header}.cpp")

    target_link_libraries(render_rd3_header_${header} PRIVATE render_runtime)

    if(header STREQUAL "NativeAssembly" OR header STREQUAL "DeviceSession")
        target_link_libraries(render_rd3_header_${header} PRIVATE render_vulkan_graph)
    else()
        file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/rd3_header_${header}.cpp"
            "#ifdef VK_VERSION_1_0\n#error neutral_runtime_header_includes_vulkan\n#endif\n"
        )
    endif()
endforeach()

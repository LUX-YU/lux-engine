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
        "-DCONSUMER_ASSET_DIR=${CMAKE_CURRENT_BINARY_DIR}/assets"
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

foreach(header Error DeviceSession DescriptorArray CompiledGraph ExecutionContext StorageBinding device/Device device/DeviceOptions memory/Memory
    Readback transfer/Submission transfer/Transfer transfer/GpuWork
    texture/TextureStorage texture/TextureCommands mesh/MeshStorage mesh/MeshCommands material/MaterialStorage material/MaterialCommands instance/InstanceStorage instance/InstanceCommands retirement/Retirement
)
    string(REPLACE "/" "_" target "${header}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${target}.cpp"
        CONTENT "#include <lux/engine/render/native/vulkan/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
    )
    add_library(render_native_header_${target} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${target}.cpp")
    if(header MATCHES "^(texture|mesh|material)/")
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

add_executable(render_color_only_test ColorOnly.cpp)

target_link_libraries(render_color_only_test PRIVATE render_native_instrumented)

add_test(NAME render.native.color_only COMMAND render_color_only_test)

set_tests_properties(render.native.color_only PROPERTIES TIMEOUT 60)

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

target_link_libraries(render_storage_bindings_test PRIVATE render_native_instrumented)

target_compile_definitions(render_storage_bindings_test PRIVATE LUX_VULKAN_TEST_SEAM)

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

add_executable(render_structured_block_test StructuredBlock.cpp)

target_link_libraries(render_structured_block_test PRIVATE render_native_vulkan_extensions render_features)

set_target_properties(render_structured_block_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin"
)

add_test(NAME render.native.structured_block COMMAND render_structured_block_test)

set_tests_properties(render.native.structured_block PROPERTIES TIMEOUT 60)

add_executable(render_parameters_test Parameters.cpp Allocation.cpp)

target_link_libraries(render_parameters_test PRIVATE render_native_vulkan_extensions render_features)

set_target_properties(render_parameters_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin"
)

add_test(NAME render.native.parameters COMMAND render_parameters_test)

set_tests_properties(render.native.parameters PROPERTIES TIMEOUT 60)

# Offline fixture asset generation reuses the existing tool implementation. No Render library
# links Engine/toolchain or the emitter; only this build-time executable does.
get_filename_component(render_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

add_executable(render_pass_asset_tool
    assets/EmitPass.cpp
    "${render_source_root}/engine/toolchain/shader/src/lglsl/LglslEmitter.cpp"
)

target_include_directories(render_pass_asset_tool PRIVATE
    "${render_source_root}/engine/toolchain/shader/include"
    "${render_source_root}/modules/resource/description/include"
    "${LUX_GENERATE_HEADER_DIR}"
)

target_link_libraries(render_pass_asset_tool PRIVATE lux::cxx::core)

if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR "The real Pass asset fixture requires glslangValidator")
endif()

set(render_asset_dir "${CMAKE_CURRENT_BINARY_DIR}/assets")

foreach(stage vert frag)
    add_custom_command(
        OUTPUT "${render_asset_dir}/Parameters.${stage}.spv"
        BYPRODUCTS "${render_asset_dir}/Parameters.${stage}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${render_asset_dir}"
        COMMAND render_pass_asset_tool
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Parameters.${stage}.lglsl"
            "${render_asset_dir}/Parameters.${stage}"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S ${stage}
            "-I${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "${render_asset_dir}/Parameters.${stage}" -o "${render_asset_dir}/Parameters.${stage}.spv"
        DEPENDS render_pass_asset_tool
            "assets/Parameters.${stage}.lglsl" assets/Parameters.shared.lglslh
        VERBATIM
    )
endforeach()

add_custom_command(
    OUTPUT "${render_asset_dir}/manifest.txt"
    COMMAND "${CMAKE_COMMAND}"
        "-DCOMPILER=${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" "-DEMITTER=$<TARGET_FILE:render_pass_asset_tool>"
        "-DROOT=${render_source_root}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "-DGENERATED=${render_asset_dir}" "-DOUTPUT=${render_asset_dir}/manifest.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/assets/Manifest.cmake"
    DEPENDS "${render_asset_dir}/Parameters.vert.spv" "${render_asset_dir}/Parameters.frag.spv"
        assets/Manifest.cmake
    VERBATIM
)

add_custom_target(render_pass_assets ALL DEPENDS "${render_asset_dir}/manifest.txt"
)

add_dependencies(render_parameters_test render_pass_assets)

add_test(NAME render.native.lglsl_parameters COMMAND render_parameters_test
    "${render_asset_dir}/Parameters.vert.spv" "${render_asset_dir}/Parameters.frag.spv"
)

set_tests_properties(render.native.lglsl_parameters PROPERTIES TIMEOUT 60)

foreach(asset IN ITEMS
    Compute.comp ComputeUpdate.comp Compute.frag
    Geometry.comp Geometry.vert Geometry.frag Depth.vert Depth.frag
)
    add_custom_command(
        OUTPUT "${render_asset_dir}/${asset}.spv"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${render_asset_dir}"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.2
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/${asset}" -o "${render_asset_dir}/${asset}.spv"
        DEPENDS "assets/${asset}"
        VERBATIM
    )
endforeach()

add_executable(render_compute_test Compute.cpp Allocation.cpp
    "${render_asset_dir}/Compute.comp.spv" "${render_asset_dir}/ComputeUpdate.comp.spv"
    "${render_asset_dir}/Compute.frag.spv"
)

target_link_libraries(render_compute_test PRIVATE render_native_vulkan)

add_test(NAME render.native.compute COMMAND render_compute_test
    "${render_asset_dir}/Compute.comp.spv" "${render_asset_dir}/ComputeUpdate.comp.spv"
    "${render_asset_dir}/Compute.frag.spv"
)

set_tests_properties(render.native.compute PROPERTIES TIMEOUT 60)

add_executable(render_geometry_test Geometry.cpp Allocation.cpp
    "${render_asset_dir}/Geometry.comp.spv" "${render_asset_dir}/Geometry.vert.spv"
    "${render_asset_dir}/Geometry.frag.spv"
)

target_link_libraries(render_geometry_test PRIVATE render_native_vulkan)

add_test(NAME render.native.geometry COMMAND render_geometry_test
    "${render_asset_dir}/Geometry.comp.spv" "${render_asset_dir}/Geometry.vert.spv"
    "${render_asset_dir}/Geometry.frag.spv"
)

set_tests_properties(render.native.geometry PROPERTIES TIMEOUT 60)

add_executable(render_depth_test Depth.cpp Allocation.cpp
    "${render_asset_dir}/Depth.vert.spv" "${render_asset_dir}/Depth.frag.spv"
)

target_link_libraries(render_depth_test PRIVATE render_native_vulkan)

add_test(NAME render.native.depth COMMAND render_depth_test
    "${render_asset_dir}/Depth.vert.spv" "${render_asset_dir}/Depth.frag.spv"
)

set_tests_properties(render.native.depth PROPERTIES TIMEOUT 60)

add_executable(render_mesh_storage_test MeshStorage.cpp Allocation.cpp
    "${render_asset_dir}/Geometry.vert.spv" "${render_asset_dir}/Geometry.frag.spv"
)

target_link_libraries(render_mesh_storage_test PRIVATE render_native_instrumented)

set_target_properties(render_mesh_storage_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_test(NAME render.native.mesh_storage COMMAND render_mesh_storage_test
    "${render_asset_dir}/Geometry.vert.spv" "${render_asset_dir}/Geometry.frag.spv"
)

set_tests_properties(render.native.mesh_storage PROPERTIES TIMEOUT 90)

add_executable(render_material_asset_tool
    assets/material/EmitMaterial.cpp
    "${render_source_root}/engine/toolchain/material/src/ShaderEmitter.cpp"
)

target_include_directories(render_material_asset_tool PRIVATE
    "${render_source_root}/engine/toolchain/material/pinclude"
)

target_link_libraries(render_material_asset_tool PRIVATE lux::engine::function::material_graph)

set_target_properties(render_material_asset_tool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin"
)

add_custom_command(
    OUTPUT "${render_asset_dir}/Unlit.frag.spv"
    BYPRODUCTS "${render_asset_dir}/Unlit.frag"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${render_asset_dir}"
    COMMAND render_material_asset_tool "${render_asset_dir}/Unlit.frag"
    COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S frag
        "-I${CMAKE_CURRENT_SOURCE_DIR}/assets/material"
        "${render_asset_dir}/Unlit.frag" -o "${render_asset_dir}/Unlit.frag.spv"
    DEPENDS render_material_asset_tool assets/material/material_types.glsl
        assets/material/texture_sampling.glsl assets/material/transition_dither.glsl
    VERBATIM
)

add_custom_command(
    OUTPUT "${render_asset_dir}/material-manifest.txt"
    COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}"
        "-DROOT=${render_source_root}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/assets/material"
        "-DGENERATED=${render_asset_dir}" "-DOUTPUT=${render_asset_dir}/material-manifest.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/assets/material/Manifest.cmake"
    DEPENDS "${render_asset_dir}/Unlit.frag.spv" "${render_asset_dir}/Unlit.vert.spv"
        "${render_asset_dir}/UnlitTexture.frag.spv" assets/material/Manifest.cmake assets/material/provenance.json
    VERBATIM
)

add_custom_target(render_material_assets ALL DEPENDS "${render_asset_dir}/material-manifest.txt")

add_custom_command(
    OUTPUT "${render_asset_dir}/Unlit.vert.spv"
    COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S vert
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/material/Unlit.vert" -o "${render_asset_dir}/Unlit.vert.spv"
    DEPENDS assets/material/Unlit.vert
    VERBATIM
)

add_custom_target(render_material_vertex ALL DEPENDS "${render_asset_dir}/Unlit.vert.spv")

add_executable(render_material_storage_test MaterialStorage.cpp Allocation.cpp)

target_link_libraries(render_material_storage_test PRIVATE render_native_instrumented lux::engine::core::core_execution)

set_target_properties(render_material_storage_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_dependencies(render_material_storage_test render_material_assets render_material_vertex)

add_test(NAME render.native.material_storage COMMAND render_material_storage_test
    "${render_asset_dir}/Unlit.vert.spv" "${render_asset_dir}/Unlit.frag.spv"
)

set_tests_properties(render.native.material_storage PROPERTIES TIMEOUT 90)

add_custom_command(
    OUTPUT "${render_asset_dir}/UnlitTexture.frag.spv"
    BYPRODUCTS "${render_asset_dir}/UnlitTexture.frag"
    COMMAND render_material_asset_tool "${render_asset_dir}/UnlitTexture.frag" texture
    COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S frag
        "-I${CMAKE_CURRENT_SOURCE_DIR}/assets/material"
        "${render_asset_dir}/UnlitTexture.frag" -o "${render_asset_dir}/UnlitTexture.frag.spv"
    DEPENDS render_material_asset_tool assets/material/material_types.glsl
        assets/material/texture_sampling.glsl assets/material/transition_dither.glsl
    VERBATIM
)

add_custom_target(render_textured_material_asset ALL DEPENDS "${render_asset_dir}/UnlitTexture.frag.spv")

add_dependencies(render_material_storage_test render_textured_material_asset)

add_test(NAME render.native.material_textures COMMAND render_material_storage_test
    "${render_asset_dir}/Unlit.vert.spv" "${render_asset_dir}/UnlitTexture.frag.spv" texture
)

set_tests_properties(render.native.material_textures PROPERTIES TIMEOUT 90)

add_executable(render_instance_storage_test InstanceStorage.cpp)

target_link_libraries(render_instance_storage_test PRIVATE render_native_instrumented lux::engine::core::core_execution)

set_target_properties(render_instance_storage_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

add_dependencies(render_instance_storage_test render_material_assets)

add_test(NAME render.native.instance_storage COMMAND render_instance_storage_test "${render_asset_dir}/Unlit.frag.spv")

set_tests_properties(render.native.instance_storage PROPERTIES TIMEOUT 120)

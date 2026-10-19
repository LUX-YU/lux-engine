find_package(Vulkan 1.3 REQUIRED)

include(cmake/ShaderTools.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine/toolchain/shader/cmake/MetadataCompiler.cmake")

include(cmake/Vma.cmake)

set(LUX_NATIVE_SOURCES
    src/Error.cpp src/DeviceSession.cpp src/Frame.cpp src/OffscreenTarget.cpp src/DescriptorArray.cpp src/device/Device.cpp src/device/Vma.cpp
    src/transfer/Submission.cpp
    src/Compile.cpp src/LayoutPlan.cpp src/Shader.cpp src/Execution.cpp
    src/transfer/UploadContext.cpp src/transfer/StagingArena.cpp
)

add_component(
    STATIC
    COMPONENT_NAME render_native_vulkan
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_native_vulkan
    SOURCE_FILES ${LUX_NATIVE_SOURCES}
)

component_include_directories(
    render_native_vulkan
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude" "${CMAKE_CURRENT_SOURCE_DIR}/sinclude" "${LUX_VMA_INCLUDE_DIR}"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_native_vulkan lux::engine::render::render_core
    lux::engine::render::render_data lux::engine::render::render_resource lux::engine::render::render_graph
)

target_link_libraries(render_native_vulkan PUBLIC
    Vulkan::Vulkan lux::engine::platform::gapi STDEXEC::stdexec lux::engine::core::core_execution
    lux_render_vma_headers
    PRIVATE lux_render_spirv_cross_c_shared lux_render_SPIRV_Tools_shared
)

component_add_transitive_commands(render_native_vulkan
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
    [=[find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS gapi
        HINTS "${CMAKE_CURRENT_LIST_DIR}/../../lux-engine-platform")]=]
    [=[include("${CMAKE_CURRENT_LIST_DIR}/ShaderTools.cmake")]=]
)

install(FILES cmake/ShaderTools.cmake cmake/Vma.cmake DESTINATION share/lux-engine-render/render_native_vulkan)

target_compile_features(render_native_vulkan PUBLIC cxx_std_20)
target_compile_definitions(render_native_vulkan PUBLIC LUX_GAPI_ENABLE_VMA=1)
component_add_transitive_commands(render_native_vulkan
    [=[include("${CMAKE_CURRENT_LIST_DIR}/Vma.cmake")]=]
)

set(LUX_NATIVE_EXTENSION_SOURCES
    src/memory/BufferArena.cpp
    src/texture/TextureStorage.cpp src/texture/Commands.cpp
    src/material/MaterialStorage.cpp src/material/Commands.cpp
    src/mesh/MeshInstanceStorage.cpp src/mesh/InstanceCommands.cpp
    src/mesh/MeshStorage.cpp src/mesh/Commands.cpp
)

add_component(
    STATIC
    COMPONENT_NAME render_native_vulkan_extensions
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_native_vulkan_extensions
    SOURCE_FILES ${LUX_NATIVE_EXTENSION_SOURCES}
)

component_include_directories(render_native_vulkan_extensions
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude" "${CMAKE_CURRENT_SOURCE_DIR}/sinclude" "${LUX_VMA_INCLUDE_DIR}"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_native_vulkan_extensions
    lux::engine::render::render_native_vulkan lux::engine::render::render_commands
)

target_compile_features(render_native_vulkan_extensions PUBLIC cxx_std_20)

# Actual Native Feature providers; Storage extensions themselves do not depend on Feature assembly.
if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR "Native Mesh Feature shaders require glslangValidator")
endif()

set(mesh_shader_headers)
foreach(shader BasicMesh DepthOnly BasicMeshPaged DepthOnlyPaged)
    set(defines)
    if(shader MATCHES "^DepthOnly")
        set(defines -DDEPTH_ONLY=1)
    endif()
    if(shader MATCHES "Paged$")
        list(APPEND defines -DPAGED_INSTANCES=1)
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}Vertex.hpp")
    set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}Vertex.spv")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${binary}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S vert
            ${defines} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Mesh.vert" -o "${binary}"
        COMMAND lux_shader_metadata "${binary}" "${header}" k${shader}Vertex
        DEPENDS shaders/Mesh.vert lux_shader_metadata
        VERBATIM
    )
    list(APPEND mesh_shader_headers "${header}")
endforeach()

add_component(
    STATIC
    COMPONENT_NAME render_native_vulkan_features
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_native_vulkan_features
    SOURCE_FILES src/mesh/MeshFeature.cpp src/features/MeshDraws.cpp ${mesh_shader_headers}
)

component_include_directories(render_native_vulkan_features
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders" "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_native_vulkan_features
    lux::engine::render::render_native_vulkan_extensions lux::engine::render::render_features
)

target_link_libraries(render_native_vulkan_features PUBLIC lux::engine::resource::shader_metadata)

component_add_transitive_commands(render_native_vulkan_features
    [=[find_package(lux-engine-resource CONFIG REQUIRED COMPONENTS shader_metadata
        HINTS "${CMAKE_CURRENT_LIST_DIR}/../../lux-engine-resource")]=]
)

target_compile_features(render_native_vulkan_features PUBLIC cxx_std_20)

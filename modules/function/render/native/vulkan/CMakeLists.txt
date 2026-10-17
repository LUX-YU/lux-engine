find_package(Vulkan 1.3 REQUIRED)

include(cmake/ShaderTools.cmake)

find_path(LUX_VMA_INCLUDE_DIR vk_mem_alloc.h
    HINTS "$ENV{VULKAN_SDK}/Include/vma" "${Vulkan_INCLUDE_DIR}/vma"
    REQUIRED
)

set(LUX_NATIVE_SOURCES
    src/Error.cpp src/DeviceSession.cpp src/DescriptorArray.cpp src/device/Device.cpp src/device/Vma.cpp
    src/memory/Memory.cpp src/transfer/Submission.cpp src/retirement/Retirement.cpp
    src/Compile.cpp src/LayoutPlan.cpp src/Shader.cpp src/Execution.cpp src/Readback.cpp
    src/transfer/Transfer.cpp src/transfer/GpuWork.cpp
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

target_link_libraries(render_native_vulkan PUBLIC Vulkan::Vulkan
    PRIVATE lux_render_spirv_cross_c_shared lux_render_SPIRV_Tools_shared lux::engine::platform::gapi
)

component_add_transitive_commands(render_native_vulkan
    [=[find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS gapi
        HINTS "${CMAKE_CURRENT_LIST_DIR}/../../lux-engine-platform")]=]
    [=[include("${CMAKE_CURRENT_LIST_DIR}/ShaderTools.cmake")]=]
)

install(FILES cmake/ShaderTools.cmake DESTINATION share/lux-engine-render/render_native_vulkan)

target_compile_features(render_native_vulkan PUBLIC cxx_std_20)

set(LUX_NATIVE_EXTENSION_SOURCES
    src/texture/TextureStorage.cpp src/texture/Commands.cpp
    src/material/MaterialStorage.cpp src/material/Commands.cpp
    src/instance/InstanceStorage.cpp src/instance/Commands.cpp
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
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude" "${CMAKE_CURRENT_SOURCE_DIR}/sinclude"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_native_vulkan_extensions
    lux::engine::render::render_native_vulkan lux::engine::render::render_protocol_handlers
)

target_compile_features(render_native_vulkan_extensions PUBLIC cxx_std_20)

# Actual Native Feature providers; Storage extensions themselves do not depend on Feature assembly.
if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR "Native Mesh Feature shaders require glslangValidator")
endif()

set(mesh_shader_headers)
foreach(shader BasicMesh DepthOnly)
    set(defines)
    if(shader STREQUAL "DepthOnly")
        set(defines -DDEPTH_ONLY=1)
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}Vertex.hpp")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S vert
            ${defines} --vn k${shader}Vertex "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Mesh.vert" -o "${header}"
        DEPENDS shaders/Mesh.vert
        VERBATIM
    )
    list(APPEND mesh_shader_headers "${header}")
endforeach()

add_component(
    STATIC
    COMPONENT_NAME render_native_vulkan_features
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_native_vulkan_features
    SOURCE_FILES src/features/MeshFeature.cpp ${mesh_shader_headers}
)

component_include_directories(render_native_vulkan_features
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_native_vulkan_features
    lux::engine::render::render_native_vulkan_extensions lux::engine::render::render_features
)

target_compile_features(render_native_vulkan_features PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

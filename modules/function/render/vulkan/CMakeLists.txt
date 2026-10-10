find_package(Vulkan 1.3 REQUIRED)

find_package(VulkanMemoryAllocator 3 CONFIG REQUIRED)

set(LUX_VULKAN_SOURCES
    src/Error.cpp
    src/device/Device.cpp
    src/device/Vma.cpp
    src/memory/Memory.cpp
    src/descriptor/Descriptors.cpp
    src/descriptor/ImageBindings.cpp
    src/pipeline/Objects.cpp
    src/pipeline/Pipeline.cpp
    src/pipeline/Graphics.cpp
    src/transfer/Submission.cpp
    src/transfer/Transfer.cpp
    src/retirement/Retirement.cpp
)

add_component(
    STATIC
    COMPONENT_NAME render_vulkan
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_vulkan
    SOURCE_FILES ${LUX_VULKAN_SOURCES}
)

component_include_directories(
    render_vulkan
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
    INSTALL_TIME include
)

target_link_libraries(
    render_vulkan
    PUBLIC lux::engine::render::render_core Vulkan::Vulkan
    PRIVATE GPUOpen::VulkanMemoryAllocator
)

target_compile_features(render_vulkan PUBLIC cxx_std_20)

# Cold Shader/Pipeline compilation consumes neutral metadata without widening Foundation's DAG.
add_library(render_vulkan_shader_compiler STATIC src/shader/Layout.cpp src/shader/Program.cpp src/shader/Bindings.cpp)

target_include_directories(
    render_vulkan_shader_compiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../engine/toolchain/shader/include"
)

target_link_libraries(
    render_vulkan_shader_compiler
    PUBLIC render_vulkan shader_metadata render_graph
    PRIVATE toolchain_shader_reflection
)

target_compile_features(render_vulkan_shader_compiler PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

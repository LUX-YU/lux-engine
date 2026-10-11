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

# Native plan ownership and execution have no reflection/link dependency. Offline
# shader creation is a separate cold compilation target in this same component.
set(LUX_VULKAN_GRAPH_SOURCES
    src/shader/Layout.cpp src/shader/Bindings.cpp src/shader/ProgramValue.cpp
    src/graph/Compile.cpp src/graph/Copy.cpp src/graph/Resources.cpp src/graph/Recipes.cpp
    src/graph/Sync.cpp src/graph/Record.cpp src/graph/Frame.cpp src/graph/Export.cpp src/graph/Diagnostics.cpp
)

add_library(render_vulkan_graph STATIC ${LUX_VULKAN_GRAPH_SOURCES})

target_include_directories(
    render_vulkan_graph
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../engine/toolchain/shader/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
)

target_link_libraries(render_vulkan_graph PUBLIC render_vulkan shader_metadata render_graph)

target_compile_features(render_vulkan_graph PUBLIC cxx_std_20)

add_library(render_vulkan_shader_compiler STATIC src/shader/Program.cpp)

target_include_directories(render_vulkan_shader_compiler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude")

target_link_libraries(
    render_vulkan_shader_compiler
    PUBLIC render_vulkan_graph
    PRIVATE toolchain_shader_reflection
)

target_compile_features(render_vulkan_shader_compiler PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

find_package(Vulkan 1.3 REQUIRED)

find_path(LUX_VMA_INCLUDE_DIR vk_mem_alloc.h
    HINTS "$ENV{VULKAN_SDK}/Include/vma" "${Vulkan_INCLUDE_DIR}/vma"
    REQUIRED
)

set(LUX_NATIVE_SOURCES
    src/Error.cpp src/DeviceSession.cpp src/device/Device.cpp src/device/Vma.cpp
    src/memory/Memory.cpp src/transfer/Submission.cpp src/retirement/Retirement.cpp
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
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude" "${LUX_VMA_INCLUDE_DIR}"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_native_vulkan lux::engine::render::render_core)

target_link_libraries(render_native_vulkan PUBLIC Vulkan::Vulkan)

component_add_transitive_commands(render_native_vulkan "find_package(Vulkan 1.3 REQUIRED)")

target_compile_features(render_native_vulkan PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

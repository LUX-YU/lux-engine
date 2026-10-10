add_executable(render_vulkan_device Device.cpp)

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


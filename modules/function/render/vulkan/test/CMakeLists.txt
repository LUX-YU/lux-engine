add_executable(render_vulkan_device Device.cpp)

target_link_libraries(render_vulkan_device PRIVATE lux::engine::render::render_vulkan)

add_test(NAME render.vulkan.device COMMAND render_vulkan_device)

set_tests_properties(render.vulkan.device PROPERTIES RUN_SERIAL TRUE)

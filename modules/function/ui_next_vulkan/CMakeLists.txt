add_component(
    COMPONENT_NAME ui_next_vulkan
    STATIC
    NAMESPACE lux::engine::function
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/UiVulkanIntegration.cpp
)

component_include_directories(
    ui_next_vulkan
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(ui_next_vulkan PUBLIC LUX_FUNCTION_DLL_DISABLE)
target_link_libraries(
    ui_next_vulkan
    PUBLIC ui_next
    PRIVATE render_client render_vulkan render_features
)

component_add_transitive_commands(
    ui_next_vulkan
    "find_package(lux-engine-function REQUIRED COMPONENTS ui_next render_client render_vulkan render_features)"
)

lux_classify_target(
    TARGET ui_next_vulkan
    LAYER FUNCTION
    PRODUCT RUNTIME
    ROLE INTEGRATION
)

if(ENABLE_UI_NEXT_TEST)
    add_executable(ui_next_vulkan_smoke test/ui_next_vulkan_smoke.cpp)
    target_link_libraries(ui_next_vulkan_smoke PRIVATE ui_next_vulkan)
    add_test(NAME ui_next_vulkan_smoke COMMAND ui_next_vulkan_smoke)
endif()

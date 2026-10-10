include(${CMAKE_CURRENT_SOURCE_DIR}/../../render/cmake/engine_render_codegen.cmake)
generate_visibility_header(
    ENABLE_MACRO_NAME LUX_UI_RENDERING_LIBRARY
    PUBLIC_MACRO_NAME LUX_UI_RENDERING_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_UI_RENDERING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/ui/rendering/visibility.h
)

add_component(
    COMPONENT_NAME ui_rendering
    NAMESPACE lux::engine::function
    SOURCE_FILES
        src/RenderClient.cpp
        src/RenderFeature.cpp
        src/VulkanBackend.cpp
)

component_include_directories(
    ui_rendering
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../sinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../../render/vulkan/sinclude
    INSTALL_TIME include
)

target_compile_definitions(ui_rendering PRIVATE LUX_UI_RENDERING_LIBRARY)
target_link_libraries(ui_rendering PUBLIC ui render_client PRIVATE imgui::vulkan Vulkan::Vulkan render_vulkan)
component_add_internal_dependencies(ui_rendering ui render_client)
component_add_transitive_commands(
    ui_rendering
    "find_package(lux-engine-function REQUIRED COMPONENTS ui render_client)"
)

engine_add_comm_ops(
    NAME ui_render_ops
    CLIENT_TARGET ui_rendering
    IMPLEMENTATION_TARGET ui_rendering
    INCLUDE_PREFIX lux/engine/ui/rendering/genops
    CLIENT_EXPORT_MACRO LUX_UI_RENDERING_PUBLIC
    CLIENT_VISIBILITY_HEADER lux/engine/ui/rendering/visibility.h
    BACKEND_EXPORT_MACRO LUX_UI_RENDERING_PUBLIC
    BACKEND_VISIBILITY_HEADER lux/engine/ui/rendering/visibility.h
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ui/rendering/UiRenderOperation.hpp
)

if(BUILD_TESTING AND LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(
        ui_sampler_ownership_test
        test/sampler_ownership.cpp
    )

    target_include_directories(
        ui_sampler_ownership_test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
            "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../render/vulkan/sinclude"
    )

    target_link_libraries(
        ui_sampler_ownership_test
        PRIVATE ui_rendering render_vulkan imgui::vulkan Vulkan::Vulkan
    )

    lux_editor_test_options(ui_sampler_ownership_test)
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME ui.sampler_ownership COMMAND ui_sampler_ownership_test)
        set_tests_properties(ui.sampler_ownership PROPERTIES TIMEOUT 60 LABELS "gpu;lifecycle")
    endif()
endif()

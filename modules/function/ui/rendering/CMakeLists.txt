include(${CMAKE_CURRENT_SOURCE_DIR}/../../render/cmake/engine_render_codegen.cmake)
generate_visibility_header(ENABLE_MACRO_NAME LUX_UI_RENDERING_LIBRARY
    PUBLIC_MACRO_NAME LUX_UI_RENDERING_PUBLIC DISABLE_DLL_MACRO_NAME LUX_UI_RENDERING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/ui/rendering/visibility.h)
add_component(COMPONENT_NAME ui_rendering NAMESPACE lux::engine::function
    SOURCE_FILES src/UiRenderClient.cpp src/UiRenderFeature.cpp src/UiVulkanBackend.cpp)
component_include_directories(ui_rendering
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${CMAKE_CURRENT_SOURCE_DIR}/../sinclude
    INSTALL_TIME include)
target_compile_definitions(ui_rendering PRIVATE LUX_UI_RENDERING_LIBRARY)
target_link_libraries(ui_rendering PUBLIC ui render_runtime PRIVATE imgui::vulkan Vulkan::Vulkan render_vulkan)
component_add_internal_dependencies(ui_rendering ui render_runtime)
component_add_transitive_commands(ui_rendering
    "find_package(lux-engine-function REQUIRED COMPONENTS ui render_runtime)")
engine_add_comm_ops(NAME ui_render_ops CLIENT_TARGET ui_rendering IMPLEMENTATION_TARGET ui_rendering
    INCLUDE_PREFIX lux/engine/ui/rendering/genops
    CLIENT_EXPORT_MACRO LUX_UI_RENDERING_PUBLIC CLIENT_VISIBILITY_HEADER lux/engine/ui/rendering/visibility.h
    BACKEND_EXPORT_MACRO LUX_UI_RENDERING_PUBLIC BACKEND_VISIBILITY_HEADER lux/engine/ui/rendering/visibility.h
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ui/rendering/UiRenderOperation.hpp)

if(BUILD_TESTING)
    add_executable(ui_render_feature_test test/feature.cpp)
    target_link_libraries(ui_render_feature_test PRIVATE ui_rendering render_features imgui::core Vulkan::Vulkan)
    if(MSVC)
        target_compile_options(ui_render_feature_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(ui_render_feature_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ui.render_feature COMMAND ui_render_feature_test)
    add_test(NAME ui.render_camera_gate COMMAND ui_render_feature_test camera)
endif()

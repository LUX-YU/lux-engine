generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_RENDERING_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_RENDERING_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_RENDERING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/rendering/visibility.h
)
add_component(COMPONENT_NAME editor_rendering NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_rendering
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EditorRenderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderView.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ViewResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ViewImage.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/EditorRenderServer.cpp
)
component_include_directories(editor_rendering
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_rendering PRIVATE LUX_EDITOR_RENDERING_LIBRARY
    LUX_EDITOR_PACKED_CONTENT=$<BOOL:${LUX_BUILD_PACKED_RENDER_CONTENT}>)
target_link_libraries(editor_rendering
    PUBLIC lux::engine::scene::scene_render lux::engine::function::ui
    PRIVATE lux::engine::function::render_vulkan lux::engine::function::render_features
        lux::engine::function::ui_vulkan_backend lux::engine::platform::window
)
component_add_transitive_commands(editor_rendering
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
)

install_components(PROJECT_NAME lux-engine-editor-rendering VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_rendering)

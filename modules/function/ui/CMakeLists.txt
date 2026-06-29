## function::ui — the reusable ImGui UI framework.
##
## This is a RUNTIME-layer component (not editor-only): panel system, docking,
## widget dispatch, the reflection-driven inspector, the texture viewport, and
## the ImGui <-> render-graph bridge. It depends on imgui + function::render
## + function::gameplay (for `ComponentTypeRegistry` — the InspectorPanel
## auto-discovers ECS components from there) + platform::window + core::meta
## + lux-cxx. It deliberately does NOT depend on flowforge / script / asset /
## node_editor / MLIR, so it can be linked on its own to build standalone UI
## applications.
##
## The engine-coupled editor panels (node editor, lua console, asset browser)
## live in engine/editor and link this component.

find_package(imgui   REQUIRED COMPONENTS glfw3 vulkan)
find_package(lux-cxx REQUIRED COMPONENTS container reflection_runtime event)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_FUNCTION_UI_LIBRARY
    PUBLIC_MACRO_NAME       LUX_FUNCTION_UI_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_FUNCTION_UI_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/function/visibility_ui.h
)

set(LUX_UI_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UISystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiDrawDataSnapshot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UIOffscreenImagePool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UIRenderSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UIRenderServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UiAppHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WidgetDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneViewportPanel.cpp
)

add_component(
    COMPONENT_NAME ui
    STATIC
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${LUX_UI_SRCS}
)

component_include_directories(
    ui
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME
        include
)

target_compile_definitions(
    ui
    PRIVATE
    LUX_FUNCTION_UI_LIBRARY
    PUBLIC
    LUX_FUNCTION_UI_DLL_DISABLE
)

# render is a SAME-LAYER dependency (function layer), so it is declared with
# component_add_internal_dependencies, NOT target_link_libraries. (gameplay was
# here only for InspectorPanel's `ComponentTypeRegistry` access; that panel moved
# to engine/editor, so ui no longer depends on the ECS layer — it is back to a
# domain-free imgui/render/Panel/Widget framework.)
component_add_internal_dependencies(ui render)

component_add_transitive_commands(
    ui
    "find_package (imgui REQUIRED COMPONENTS glfw3 vulkan)"
    "find_package (lux-cxx REQUIRED COMPONENTS container reflection_runtime event)"
)

target_link_libraries(
    ui
    PUBLIC
    imgui::glfw3
    imgui::vulkan
    lux::cxx::container
    lux::cxx::reflection_runtime
    lux::cxx::event                        # SceneViewportPanel resized/picked/asset_dropped signals
)

set(ENABLE_UI_TEST OFF CACHE BOOL "Enable UI Test")
if(ENABLE_UI_TEST)
    add_subdirectory(test)
endif()

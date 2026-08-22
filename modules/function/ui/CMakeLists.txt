## function::ui — backend-neutral ImGui UI state and frame-client support.
##
## function::ui_vulkan is the explicit ImGui <-> Vulkan integration.  Keeping
## UIRenderServer out of ui means graph/editor widgets do not acquire a Vulkan
## backend merely by using Panel or UISystem.
##
## The engine-coupled editor panels (node editor, lua console, asset browser)
## live in engine/editor and link this component.

find_package(imgui   REQUIRED COMPONENTS glfw3 vulkan)
find_package(lux-cxx REQUIRED COMPONENTS core container reflection_runtime)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_FUNCTION_UI_LIBRARY
    PUBLIC_MACRO_NAME       LUX_FUNCTION_UI_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_FUNCTION_UI_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/function/visibility_ui.h
)

set(LUX_UI_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UISystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WidgetDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneViewportPanel.cpp
)

set(LUX_UI_VULKAN_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiDrawDataSnapshot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UIOffscreenImagePool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UIRenderFrameSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UIRenderServer.cpp
)

add_component(
    COMPONENT_NAME ui
    STATIC
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${LUX_UI_SRCS}
)

add_component(
    COMPONENT_NAME ui_vulkan
    STATIC
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${LUX_UI_VULKAN_SRCS}
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

component_include_directories(
    ui_vulkan
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

target_compile_definitions(
    ui_vulkan
    PRIVATE
        LUX_FUNCTION_UI_LIBRARY
    PUBLIC
        LUX_FUNCTION_UI_DLL_DISABLE
)

# UI frame submission is a render-client concern.  Vulkan server/factory code
# is isolated in the integration target and declares both backend dependencies.
component_add_internal_dependencies(ui render_client)
component_add_internal_dependencies(
    ui_vulkan
    ui
    render_vulkan
    render_features
)

component_add_transitive_commands(
    ui
    "find_package (imgui REQUIRED COMPONENTS glfw3 vulkan)"
    "find_package (lux-cxx REQUIRED COMPONENTS core container reflection_runtime)"
    "find_package (lux-engine-platform REQUIRED COMPONENTS window)"
)

component_add_transitive_commands(
    ui_vulkan
    "find_package (imgui REQUIRED COMPONENTS glfw3 vulkan)"
    "find_package (lux-engine-function REQUIRED COMPONENTS ui render_vulkan render_features)"
    "find_package (lux-engine-platform REQUIRED COMPONENTS window)"
)

target_link_libraries(
    ui
    PUBLIC
    imgui::glfw3
    imgui::vulkan
    lux::cxx::container
    lux::cxx::reflection_runtime
    PRIVATE
    lux::cxx::core
    lux::engine::platform::window
)

target_link_libraries(
    ui_vulkan
    PUBLIC
        imgui::glfw3
        imgui::vulkan
    PRIVATE
        lux::engine::platform::window
)

lux_classify_target(
    TARGET  ui
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

lux_classify_target(
    TARGET  ui_vulkan
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

set(ENABLE_UI_TEST OFF CACHE BOOL "Enable UI Test")
if(ENABLE_UI_TEST)
    add_subdirectory(test)
endif()

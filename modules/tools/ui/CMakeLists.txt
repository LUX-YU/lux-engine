find_package(imgui REQUIRED COMPONENTS glfw opengl3)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_TOOLS_UI_LIBRARY
    PUBLIC_MACRO_NAME       LUX_TOOLS_UI_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_TOOLS_UI_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/tools/visibility_ui.h
)

set(LUX_UI_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Widget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WindowUI.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Scene3DWidget.cpp
)

add_component(
    COMPONENT_NAME ui
    STATIC
    NAMESPACE      lux::engine::tools
    SOURCE_FILES   ${LUX_UI_SRCS}
)

component_include_directories(
    ui
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    ui
    PRIVATE
    LUX_TOOLS_UI_LIBRARY
    PUBLIC
    LUX_TOOLS_UI_DLL_DISABLE
)

component_add_transitive_commands(
    ui
    "find_package (lux-engine-function REQUIRED COMPONENTS render)"
    "find_package (imgui REQUIRED COMPONENTS glfw opengl3)"
)

target_link_libraries(
    ui
    PUBLIC
    lux::engine::function::render
    imgui::glfw
    imgui::opengl3
)

set(IMGUI_SOURCE_DIR            ${THIRD_PARTY_SOURCE_DIR}/imgui/imgui-1.88)
set(IMGUI_SOURCE_BACKEND_DIR    ${IMGUI_SOURCE_DIR}/backends)

set(IMGUI_COMMON_SRCS
    ${IMGUI_SOURCE_DIR}/imgui_demo.cpp
    ${IMGUI_SOURCE_DIR}/imgui_draw.cpp
    ${IMGUI_SOURCE_DIR}/imgui_tables.cpp
    ${IMGUI_SOURCE_DIR}/imgui_widgets.cpp
    ${IMGUI_SOURCE_DIR}/imgui.cpp
)

set(IMGUI_OPENGL3_BACKEND
    ${IMGUI_SOURCE_BACKEND_DIR}/imgui_impl_opengl3.cpp
    ${IMGUI_SOURCE_BACKEND_DIR}/imgui_impl_glfw.cpp
)

# glad
set(GLAD_SOURCE_DIR     ${THIRD_PARTY_SOURCE_DIR}/glad)
set(GLAD_INCLUDE_DIR    ${GLAD_SOURCE_DIR}/include)
set(GLAD_SRCS           ${GLAD_SOURCE_DIR}/src/glad.c)

# opengl
find_package(OpenGL REQUIRED)


if(WIN32)
    set(IMGUI_DIRECTX12_BACKEND
        ${IMGUI_SOURCE_BACKEND_DIR}/imgui_impl_win32.cpp
        ${IMGUI_SOURCE_BACKEND_DIR}/imgui_impl_dx12.cpp
    )
endif()

add_module(
    MODULE_NAME                 gui
    NAMESPACE                   lux::engine::platform
    SOURCE_FILES                ${IMGUI_COMMON_SRCS}
                                ${IMGUI_OPENGL3_BACKEND}
                                ${IMGUI_DIRECTX12_BACKEND}
                                ${GLAD_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PROJECT_SHARED_INCLUDE_DIRS ${IMGUI_SOURCE_DIR}
                                ${IMGUI_SOURCE_BACKEND_DIR}
                                ${GLAD_INCLUDE_DIR}
                                ${CMAKE_CURRENT_SOURCE_DIR}/shared_include
    PUBLIC_LIBRARIES            lux::engine::platform::window
                                OpenGL::GL
)

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

# directx 12
if(WIN32)
    set(IMGUI_DIRECTX12_BACKEND
        ${IMGUI_SOURCE_BACKEND_DIR}/imgui_impl_win32.cpp
        ${IMGUI_SOURCE_BACKEND_DIR}/imgui_impl_dx12.cpp
    )
endif()

set(LUX_ENGINE_RENDER_SRCS
    src/opengl3_shader.cpp
    src/opengl3_shader_program.cpp
    src/Camera.cpp
    src/CameraHelper.cpp
)

add_module(
    MODULE_NAME                 render
    NAMESPACE                   function
    SOURCE_FILES                ${LUX_ENGINE_RENDER_SRCS}
                                ${IMGUI_COMMON_SRCS}
                                ${IMGUI_OPENGL3_BACKEND}
                                ${GLAD_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PUBLIC_LIBRARIES            lux::engine::core::math
                                lux::engine::platform::system
                                lux::engine::platform::window
                                lux::engine::resource::image
                                lux::engine::resource::model
                                OpenGL::GL
    PROJECT_SHARED_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
                                ${CMAKE_CURRENT_SOURCE_DIR}/shared_include
                                ${IMGUI_SOURCE_DIR}
                                ${IMGUI_SOURCE_BACKEND_DIR} 
                                ${GLAD_INCLUDE_DIR}
)


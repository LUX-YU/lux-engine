# find_library(IMGUI_LIBRARY imgui REQUIRED)
find_library(GLAD_LIBRARY  glad  REQUIRED)
# opengl
find_package(OpenGL REQUIRED)

set(GRAPHIC_API_SRCS
    src/graphic_api/opengl3/opengl3_shader.cpp
    src/graphic_api/opengl3/opengl3_shader_program.cpp
)

set(RENDER_SRCS
    src/render/Camera.cpp
    src/render/CameraHelper.cpp
    src/render/GodHand.cpp
    src/render/GodHandOpenGLImpl.cpp
)

add_module(
    MODULE_NAME                 render
    NAMESPACE                   function
    SOURCE_FILES                ${GRAPHIC_API_SRCS}
                                ${RENDER_SRCS}
    EXPORT_INCLUDE_DIRS         include
    PRIVATE_INCLUDE_DIRS        ${CMAKE_PREFIX_PATH}/include
    PUBLIC_LIBRARIES            lux::engine::core::math
                                lux::engine::platform::system
                                lux::engine::platform::window
                                lux::engine::resource::image
                                lux::engine::resource::model
                                OpenGL::GL
    PRIVATE_LIBRARIES           # ${IMGUI_LIBRARY}
                                ${GLAD_LIBRARY}
)

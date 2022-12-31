# find_library(IMGUI_LIBRARY imgui REQUIRED)
find_library(GLAD_LIBRARY   glad  REQUIRED)
find_path(GLAD_INCLUDE      glad/glad.h REQUIRED)
# opengl
find_package(OpenGL     REQUIRED)
find_package (Eigen3    REQUIRED)
set(GRAPHIC_API_SRCS
    src/opengl3/shader.cpp
    src/opengl3/shader_program.cpp
)

set(RHI_SRCS
    src/RHI_GL4.cpp
    src/GraphicDeviceInfo.cpp
)

add_module(
    MODULE_NAME             RHI
    NAMESPACE               platform
    SOURCE_FILES            ${RHI_SRCS}
                            ${GRAPHIC_API_SRCS}
    EXPORT_INCLUDE_DIRS     include
    PRIVATE_INCLUDE_DIRS    ${GLAD_INCLUDE}
    PUBLIC_LIBRARIES        lux::engine::platform::system
                            Eigen3::Eigen
                            OpenGL::GL
                            ${GLAD_LIBRARY}
)

if(WIN32)
target_link_libraries(
        RHI
        PRIVATE
        wbemuuid
)
endif()
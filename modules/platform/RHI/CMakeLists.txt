# find_library(IMGUI_LIBRARY imgui REQUIRED)
find_library(GLAD_LIBRARY   glad  REQUIRED)
find_path(GLAD_INCLUDE      glad/glad.h REQUIRED)
# opengl
find_package(OpenGL     REQUIRED)
set(RHI_SRCS
    src/RHI_GL4.cpp
    src/GraphicDeviceInfo.cpp
)

add_module(
    MODULE_NAME             RHI
    NAMESPACE               platform
    SOURCE_FILES            ${RHI_SRCS}
    EXPORT_INCLUDE_DIRS     include
    PRIVATE_INCLUDE_DIRS    ${GLAD_INCLUDE}
    PUBLIC_LIBRARIES        lux::engine::platform::system
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
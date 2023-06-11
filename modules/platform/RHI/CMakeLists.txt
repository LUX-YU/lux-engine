# find_library(IMGUI_LIBRARY imgui REQUIRED)
find_library(GLAD_LIBRARY   glad        REQUIRED)
find_path(GLAD_INCLUDE      glad/glad.h REQUIRED)
# opengl
find_package(OpenGL     REQUIRED)
set(RHI_SRCS
    src/RHI_GL4.cpp
    src/GraphicDeviceInfo.cpp
)

add_component(
    COMPONENT_NAME                  RHI
    NAMESPACE                       lux::engine::platform
    SOURCE_FILES                    ${RHI_SRCS}
	BUILD_TIME_EXPORT_INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME_INCLUDE_PREFIX     include
    PRIVATE_INCLUDE_DIRS            ${GLAD_INCLUDE}
    PUBLIC_LIBRARIES                OpenGL::GL
                                    ${GLAD_LIBRARY}
    PRIVATE_DEFINITIONS             LUX_PLATFORM_LIBRARY
    TRANSITIVE_PACKAGES_COMMANDS    "find_library(GLAD_LIBRARY   glad        REQUIRED)"
                                    "find_path(GLAD_INCLUDE      glad/glad.h REQUIRED)"
                                    "find_package(OpenGL     REQUIRED)"
)

if(WIN32)
target_link_libraries(
        RHI
        PRIVATE
        wbemuuid
)
endif()
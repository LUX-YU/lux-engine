# find_library(IMGUI_LIBRARY imgui REQUIRED)
find_library(GLAD_LIBRARY   glad        REQUIRED)
find_path(GLAD_INCLUDE      glad/glad.h REQUIRED)
# opengl
find_package(OpenGL     REQUIRED)
find_package(Vulkan     REQUIRED)
if(VULKAN_FOUND)
message("---- VULKAN INCLUDE DIRS: ${Vulkan_INCLUDE_DIRS}")
message("---- VULKAN LIBRARIES: ${Vulkan_LIBRARIES}")
endif()

set(RHI_OPENGL4_SRCS
    src/gl4RHI/GLRHI.cpp
    src/gl4RHI/OpenGLBuffer.cpp
)

set(RHI_SRCS
    src/RHI.cpp
    src/GraphicDeviceInfo.cpp
    ${RHI_OPENGL4_SRCS}
)

add_component(
    COMPONENT_NAME RHI
    NAMESPACE      lux::engine::platform
    SOURCE_FILES   ${RHI_SRCS}
)

component_include_directories(
    RHI
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
    PRIVATE
        ${GLAD_INCLUDE}
)

target_compile_definitions(
    RHI
    PRIVATE
    LUX_PLATFORM_LIBRARY
)

target_link_libraries(
    RHI
    PUBLIC
    OpenGL::GL
    Vulkan::Vulkan
    ${GLAD_LIBRARY}
)

component_add_transitive_commands(
    RHI
    "find_package(OpenGL REQUIRED)"
)

if(WIN32)
target_link_libraries(
        RHI
        PRIVATE
        wbemuuid
)
endif()
set(GLFW_WRAPPER_SRCS
    src/glfw_wrapper/LuxWindow.cpp
    src/glfw_wrapper/GLContext.cpp
)

find_package(glfw3 REQUIRED)

add_module(
    MODULE_NAME             window
    NAMESPACE               platform
    SOURCE_FILES            ${GLFW_WRAPPER_SRCS}
    EXPORT_INCLUDE_DIRS     include
    PRIVATE_INCLUDE_DIRS    pinclude
    PUBLIC_LIBRARIES        glfw
                            lux::engine::platform::system
)

if(WIN32)
    target_compile_definitions(
        window
        PUBLIC
        __PLATFORM_WIN32__
    )
endif()

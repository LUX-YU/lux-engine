find_package(glfw3 REQUIRED)

set(GLFW_WRAPPER_SRCS
    src/glfw_wrapper/LuxWindow.cpp
    src/glfw_wrapper/GLContext.cpp
)

add_component(
    COMPONENT_NAME                  window
    NAMESPACE                       lux::engine::platform
    SOURCE_FILES                    ${GLFW_WRAPPER_SRCS}
	BUILD_TIME_EXPORT_INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME_INCLUDE_PREFIX     include
    PRIVATE_INCLUDE_DIRS            pinclude
    PUBLIC_LIBRARIES                glfw
    PRIVATE_DEFINITIONS             LUX_PLATFORM_LIBRARY
    TRANSITIVE_PACKAGES_COMMANDS    "find_package(glfw3 REQUIRED)"
)

if(WIN32)
    target_compile_definitions(
        window
        PUBLIC
        __PLATFORM_WIN32__
    )
endif()

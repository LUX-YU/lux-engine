find_package(glfw3   REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS event)

set(WINDOW_SRCS
    src/LuxWindow.cpp
    src/GlfwRuntime.cpp
)

if(WIN32)
	list(APPEND WINDOW_SRCS src/TrayIconWin32.cpp)
endif()

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_PLATFORM_WINDOW_LIBRARY
    PUBLIC_MACRO_NAME   LUX_PLATFORM_WINDOW_PUBLIC
    GENERATE_FILE_PATH  lux/engine/window/visibility.h
)

# Component: window
# -----------------
# The window component is responsible for creating and managing the window
# Introduced dependencies:
# glfw3
# common
# lux::cxx::event
add_component(
    COMPONENT_NAME window
    NAMESPACE      lux::engine::platform
    SOURCE_FILES   ${WINDOW_SRCS}
)

component_include_directories(
    window
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
    PRIVATE
        pinclude
)

target_compile_definitions(
    window
    PRIVATE
    LUX_PLATFORM_WINDOW_LIBRARY
)

target_link_libraries(
    window
    PUBLIC
    glfw
    lux::cxx::event
)

component_add_internal_dependencies(
    window
    common
)

component_add_transitive_commands(
    window
    "find_package(glfw3 REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS event)"
)

if(WIN32)
    target_compile_definitions(
        window
        PUBLIC
        __PLATFORM_WIN32__
        __TRAY_ENABLED__
    )
endif()

# Headers only: LuxWindow::createVulkanSurface needs the Vulkan types; the
# entry point itself lives in the window backend (GLFW loads Vulkan dynamically).
find_package(Vulkan  REQUIRED)

# Backend source selection — same pattern as dynamic_library (win32/posix):
# one public header set (LuxWindow.hpp / InputSnapshot.hpp / ...), one backend
# implementation chosen at configure time. An Android backend adds
# src/LuxWindow.android.cpp (+ its runtime) here WITHOUT touching consumers —
# see .internal/lux-engine-mobile-adaptation-investigation.md §3.
if(ANDROID)
    # Android backend. Deliberately thin: the Vulkan surface does NOT come from
    # here (see LuxWindow::createVulkanSurface's comment — the native window's
    # lifetime matches a surface, not a window object, so it goes straight to
    # RenderSurface::initFromNative via the CreateSurfaceTarget command).
    # The event pump is likewise out of scope: that belongs to the app's
    # activity glue, not to an engine library.
    set(WINDOW_SRCS
        src/LuxWindow.android.cpp
    )
else()
    find_package(glfw3 REQUIRED)
    set(WINDOW_SRCS
        src/LuxWindow.glfw.cpp
        src/GlfwRuntime.cpp
    )
endif()

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
add_component(
    COMPONENT_NAME window
    NAMESPACE      lux::engine::platform
    SOURCE_FILES   ${WINDOW_SRCS}
)

lux_classify_target(
    TARGET  window
    LAYER   PLATFORM
    PRODUCT RUNTIME
    ROLE    FOUNDATION
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
    PRIVATE
    Vulkan::Headers
)

# PUBLIC: interactive tests/tools still call GLFW directly (glfwGetKey etc.).
if(NOT ANDROID)
    target_link_libraries(window PUBLIC glfw)
endif()

component_add_internal_dependencies(
    window
    common
)

component_add_transitive_commands(
    window
    "if(NOT ANDROID)"
    "    find_package(glfw3 REQUIRED)"
    "endif()"
    "find_package(Vulkan REQUIRED)"
)

if(WIN32)
    target_compile_definitions(
        window
        PUBLIC
        __PLATFORM_WIN32__
        __TRAY_ENABLED__
    )
endif()

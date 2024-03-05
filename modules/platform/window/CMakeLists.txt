find_package(glfw3   REQUIRED)
find_package(Vulkan  REQUIRED)
find_package(imgui   REQUIRED COMPONENTS imgui)

set(GLFW_WRAPPER_SRCS
    src/glfw_wrapper/LuxWindow.cpp
    src/glfw_wrapper/GLContext.cpp
    src/glfw_wrapper/VulkanContext.cpp
    src/glfw_wrapper/Subwindow.cpp
)

add_component(
    COMPONENT_NAME window
    NAMESPACE      lux::engine::platform
    SOURCE_FILES   ${GLFW_WRAPPER_SRCS}
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
    LUX_PLATFORM_LIBRARY
)

target_link_libraries(
    window
    PUBLIC
    glfw
    Vulkan::Vulkan
    imgui::imgui
)

component_add_transitive_commands(
    window
    "find_package(glfw3     REQUIRED)"
    "find_package(Vulkan    REQUIRED)"
)

if(WIN32)
    target_compile_definitions(
        window
        PUBLIC
        __PLATFORM_WIN32__
    )
endif()

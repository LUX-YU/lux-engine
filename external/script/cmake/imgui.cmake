project(imgui)
cmake_minimum_required(VERSION 3.10)

set(CMAKE_BUILD_TYPE DEBUG)

set(IMGUI_SRCS
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    imgui.cpp
)

set(IMGUI_HEADER
    imconfig.h
    imgui_internal.h
    imgui.h
    imstb_rectpack.h
    imstb_textedit.h
    imstb_truetype.h
)

# opengl
find_package(OpenGL REQUIRED)
# glfw
find_package(glfw3 REQUIRED)

# build opengl3 support 
set(IMGUI_BACKEND_SRCS
    backends/imgui_impl_opengl3.cpp
    backends/imgui_impl_glfw.cpp
)
list(APPEND IMGUI_HEADER
    backends/imgui_impl_opengl3_loader.h
    backends/imgui_impl_opengl3.h
    backends/imgui_impl_glfw.h
) 

# build directx11/12 support in windows
if(WIN32)
    list(APPEND IMGUI_BACKEND_SRCS 
        backends/imgui_impl_win32.cpp
        backends/imgui_impl_dx11.cpp
        backends/imgui_impl_dx12.cpp
    )
    list(APPEND IMGUI_HEADER
        backends/imgui_impl_win32.h
        backends/imgui_impl_dx11.h
        backends/imgui_impl_dx12.h
    ) 
endif()

add_library(
    imgui
    STATIC
    ${IMGUI_SRCS} 
    ${IMGUI_BACKEND_SRCS}
)

target_include_directories(
    imgui
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

target_link_libraries(
    imgui
    PRIVATE
    OpenGL::GL
    glfw
)

install(
    FILES ${IMGUI_HEADER}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include
)

install(
    TARGETS imgui
)

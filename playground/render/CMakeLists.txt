message("-- Enable Playgound Project: imgui_glfw_opengl3")

add_executable(
    render_test
    opengl3/init/imgui_glfw_opengl3.cpp
    opengl3/init/triangle.cpp
    opengl3/init/shaders.cpp
    opengl3/init/textures.cpp
    opengl3/init/transformations.cpp
    opengl3/init/coordinate_system.cpp
    opengl3/init/camera.cpp

    opengl3/light/color.cpp
    opengl3/light/basic_lighting.cpp

    opengl3/vertex/CubeVertex.cpp
    render_test_entry.cpp
)

target_link_libraries(
    render_test
    PRIVATE
    lux::engine::platform::gui
    lux::engine::platform::cxx
    lux::engine::platform::media_loaders
    lux::engine::core::math
    lux::engine::function::render
)

target_include_directories(
    render_test
    PRIVATE
    opengl3/vertex
)

message("-- Enable Playgound Project: imgui_glfw_opengl3")

set(CONTEXT_PATH        ${CMAKE_CURRENT_BINARY_DIR})
set(TEXTURE_PATH        ${CMAKE_CURRENT_SOURCE_DIR}/texture)
set(OPENGL_SHADER_PATH  ${CMAKE_CURRENT_SOURCE_DIR}/opengl3/shaders)
configure_file(render_context.hpp.in ${CONTEXT_PATH}/render_context.hpp) 

set(RENDER_OPENGL3_SRCS
    # opengl3/init/imgui_glfw_opengl3.cpp
    # opengl3/init/triangle.cpp
    # opengl3/init/shaders.cpp
    # opengl3/init/textures.cpp
    # opengl3/init/transformations.cpp
    # opengl3/init/coordinate_system.cpp
    # opengl3/init/camera.cpp
    # opengl3/light/color.cpp
    # opengl3/light/basic_lighting.cpp
    # opengl3/light/materials.cpp
    opengl3/light/lighting_map.cpp
    opengl3/light/light_caster.cpp
    opengl3/vertex/CubeVertex.cpp
)

set(MODEL_OPENGL3_SRCS
    opengl3/model/load_model.cpp
)

add_executable(
    render_test
    ${RENDER_OPENGL3_SRCS}
    ${MODEL_OPENGL3_SRCS}
    render_test_entry.cpp
)

target_link_libraries(
    render_test
    PRIVATE
    lux::engine::platform::cxx
    lux::engine::core::math
    lux::engine::resource::image
    lux::engine::resource::model
    lux::engine::function::render
)

target_include_directories(
    render_test
    PRIVATE
    opengl3/vertex
    ${CONTEXT_PATH}
)

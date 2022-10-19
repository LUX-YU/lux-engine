#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <functional>

#include <lux-engine/resource/loaders/Image.hpp>
#include <lux-engine/platform/window/LuxWindow.hpp>
#include <lux-engine/core/math/EigenTools.hpp>
#include <render_helper/CameraHelper.hpp>

#include <graphic_api_wrapper/opengl3/VertexBufferObject.hpp>
#include <graphic_api_wrapper/opengl3/ShaderProgram.hpp>

#include "CubeVertex.hpp"

static const char* predifined_vertex_shader =
R"(
#version 330 core
layout (location = 0) in vec3 aPos;

uniform mat4 mvp;

void main()
{
    gl_Position = mvp * vec4(aPos, 1.0f);
}
)";

static const char* predefined_fragment_shader =
R"(
#version 330 core
out vec4 FragColor;

uniform vec3 objectColor;
uniform vec3 lightColor;

void main()
{
    FragColor = vec4(lightColor * objectColor, 1.0);
}
)";

static const char* predefined_light_fragment_shader =
R"(
#version 330 core
out vec4 FragColor;

void main()
{
    FragColor = vec4(1.0);
}
)";

static int global_width  = 1920;
static int global_height = 1080;

static void glad_init()
{
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    int nrAttributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &nrAttributes);
    std::cout << "Maximum nr of vertex attributes supported: " << nrAttributes << std::endl;
}

static int __main(int argc, char* argv[])
{
    using namespace lux::engine;
    platform::LuxWindow window(global_width, global_height, "color");
    glad_init();
    
    function::ShaderProgram cube_program;
    function::ShaderProgram light_program;
    
    {
        std::string info;
        function::GlShader* shaders[2];
        function::GlVertexShader   vertex_shader(&predifined_vertex_shader);
        function::GlFragmentShader fragment_shader(&predefined_fragment_shader);
        shaders[0] = &vertex_shader;
        shaders[1] = &fragment_shader;
        for(auto shader : shaders)
        {
            bool is_success = shader->compile(info);
            if(!is_success) {std::cerr << "compile failed!" << std::endl; return -1;};
            cube_program.attachShader(*shader);
        }
        cube_program.link(info);
    }

    {
        std::string info;
        function::GlShader* shaders[2];
        function::GlVertexShader   vertex_shader(&predifined_vertex_shader);
        function::GlFragmentShader fragment_shader(&predefined_light_fragment_shader);
        shaders[0] = &vertex_shader;
        shaders[1] = &fragment_shader;
        for(auto shader : shaders)
        {
            bool is_success = shader->compile(info);
            if(!is_success) {std::cerr << "compile failed!" << std::endl; return -1;};
            light_program.attachShader(*shader);
        }
        light_program.link(info);
    }


    GLuint vbo;
    // vbo set
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube_vertex_texture), cube_vertex_texture, GL_STATIC_DRAW);

    GLuint cube_vao;
    glGenVertexArrays(1, &cube_vao);
    glBindVertexArray(cube_vao);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector5f), (void*)0);
    glEnableVertexAttribArray(0);

    // light vao
    // glBindBuffer(GL_ARRAY_BUFFER, vbo);
    GLuint light_vao;
    glGenVertexArrays(1, &light_vao);
    glBindVertexArray(light_vao);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector5f), (void*)0);
    glEnableVertexAttribArray(0);

    glEnable(GL_DEPTH_TEST);

    cube_program.use();
    cube_program.uniformSetVectorUnsafe("objectColor", std::array{1.0f, 0.5f, 0.31f});
    cube_program.uniformSetVectorUnsafe("lightColor", 1.0f, 1.0f, 1.0f);
    auto cube_mvp_location = cube_program.uniformFindLocationUnsafe("mvp");
    auto light_mvp_location= light_program.uniformFindLocationUnsafe("mvp");
    
    float deltaTime = 0.0f; // 当前帧与上一帧的时间差
    float lastFrame = 0.0f; // 上一帧的时间

    function::UserControlCamera camera(window);

    window.enableVsync(true);
    glfwSetInputMode((GLFWwindow*)window.lowLayerPointer(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // cube position
    Eigen::Affine3f cube_mode  = core::createTransform(Eigen::Vector3f{0,0,0}, {0,0,0});
    // light position
    Eigen::Affine3f light_mode = core::createTransform(Eigen::Vector3f{0,0,0}, {120.0f, 100.0f, 200.0f});

    
    while(!window.shouldClose())
    {
        float currentFrame = platform::LuxWindow::timeAfterFirstInitialization();
        deltaTime = currentFrame - lastFrame;

        camera.setCameraSpeed(5000.0f * deltaTime);
        camera.updateViewInLoop();
        
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

        cube_program.use();
        
        // can be also implement by frustumMatrix
        Eigen::Matrix4f projection_transform =  core::perspectiveMatrix(camera.fov() * EIGEN_PI/180, global_width / (float)global_height, 0.1f, 50000.0f);
        auto cube_mvp = projection_transform * camera.viewMatrix() * cube_mode;
        cube_program.uniformSetMatrix<float, 4, 4>(cube_mvp_location, false, cube_mvp.data());

        glBindVertexArray(cube_vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        light_program.use();
        auto light_mvp = projection_transform * camera.viewMatrix() * light_mode;
        light_program.uniformSetMatrix<float, 4, 4>(light_mvp_location, false, light_mvp.data());
        glBindVertexArray(light_vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        window.swapBuffer();
        platform::LuxWindow::pollEvents();
        lastFrame = currentFrame;
    }

    return 0;
}

#include <lux-engine/platform/cxx/SubProgram.hpp>
RegistFunctionSubProgram(__main, "color")

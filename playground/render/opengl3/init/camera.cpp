#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <functional>

#include <lux-engine/platform/media_loaders/Image.hpp>
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
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

uniform mat4 mvp;

void main()
{
    gl_Position = mvp * vec4(aPos, 1.0f);
    TexCoord = aTexCoord;
}
)";

static const char* predefined_fragment_shader =
R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

uniform sampler2D ourTexture;

void main()
{
    FragColor = texture(ourTexture, TexCoord);
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
    platform::LuxWindow window(global_width, global_height, "camera");
    glad_init();
    
    function::ShaderProgram shader_program;
    
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
            std::cout << "compile message:" << info << std::endl;
            if(!is_success) {std::cerr << "compile failed!" << std::endl; return -1;};
        }
        shader_program.attachShader(vertex_shader);
        shader_program.attachShader(fragment_shader);
        shader_program.link(info);
        std::cout << "link message:" << info << std::endl;
    }

    unsigned int texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    lux::engine::platform::Image image("D:/Code/lux-game/playground/render/opengl3/texture/miku.png");
    if(!image.isEnable())
    {
        std::cout << "Failed to load texture" << std::endl;
        return -1;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
    glActiveTexture(GL_TEXTURE0);

    GLuint vbo;
    glGenBuffers(1, &vbo);

    GLuint vao;
    glGenVertexArrays(1, &vao);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glBufferData(GL_ARRAY_BUFFER, sizeof(cube_vertex_texture), cube_vertex_texture, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector5f), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector5f), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glEnable(GL_DEPTH_TEST);
    shader_program.use();
    shader_program.uniformSetVectorUnsafe("outTexture", 0.0f);

    auto mvp_location = shader_program.uniformFindLocationUnsafe("mvp");

    Eigen::Vector3f cubePositions[] = {
        { 0.0f,    0.0f,   -100.0f},
        { 100.0f,  250.0f, -150.0f},
        {-150.0f, -220.0f, -250.0f},
        {-280.0f, -200.0f, -123.0f},
        { 240.0f,  -40.0f, -350.0f},
        {-170.0f,  300.0f, -750.0f},
        { 130.0f, -200.0f, -250.0f},
        { 150.0f,  200.0f, -250.0f},
        { 150.0f,  20.0f,  -150.0f},
        {-130.0f,  100.0f, -150.0f} 
    };
    
    float deltaTime = 0.0f; // 当前帧与上一帧的时间差
    float lastFrame = 0.0f; // 上一帧的时间

    function::UserControlCamera camera(window);

    window.enableVsync(true);
    glfwSetInputMode((GLFWwindow*)window.lowLayerPointer(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    while(!window.shouldClose())
    {
        float currentFrame = platform::LuxWindow::timeAfterFirstInitialization();
        deltaTime = currentFrame - lastFrame;

        camera.setCameraSpeed(10000.0f * deltaTime);
        camera.updateViewInLoop();
        
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

        float aspect = global_width / (float)global_height;

         // can be also implement by frustumMatrix
        Eigen::Matrix4f projection_transform = 
            core::perspectiveMatrix(camera.fov() * EIGEN_PI/180, aspect, 0.1f, 50000.0f);

        for(const auto& position : cubePositions)
        {
            Eigen::Affine3f model_transform = core::createTransform(
                Eigen::Vector3f{currentFrame,currentFrame,currentFrame}, 
                {position[0], position[1], position[2]}
            );

            Eigen::Projective3f mvp = projection_transform * camera.viewMatrix() * model_transform;

            shader_program.uniformSetMatrix(mvp_location, false, mvp);

            glDrawArrays(GL_TRIANGLES, 0, 36);
        }

        window.swapBuffer();
        platform::LuxWindow::pollEvents();
        lastFrame = currentFrame;
    }

    return 0;
}

#include <lux-engine/platform/cxx/SubProgram.hpp>
RegistFunctionSubProgram(__main, "camera")

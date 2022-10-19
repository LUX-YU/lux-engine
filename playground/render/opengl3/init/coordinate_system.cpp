#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include <Eigen/Geometry>
#include <stb_image.h>

#include <lux-engine/resource/loaders/Image.hpp>
#include <lux-engine/core/math/EigenTools.hpp>

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

static void sizeChangedCallback(GLFWwindow* window, int width, int height)
{
    global_width  = width;
    global_height = height;
    glViewport(0, 0, width, height);
}

static GLFWwindow* glfw_init()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

    auto window = glfwCreateWindow(global_width, global_height, "shader", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    glfwSetWindowSizeCallback(window, sizeChangedCallback);

    return window;
}

static void glad_init()
{
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    int nrAttributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &nrAttributes);
    std::cout << "Maximum nr of vertex attributes supported: " << nrAttributes << std::endl;
}

static int __main(int argc, char* argv[])
{
    auto window = glfw_init();
    glad_init();

    using namespace lux::engine;
    function::ShaderProgram shader_program;
    
    {
        std::string info;
        lux::engine::function::GlShader* shaders[2];
        lux::engine::function::GlVertexShader   vertex_shader(&predifined_vertex_shader);
        lux::engine::function::GlFragmentShader fragment_shader(&predefined_fragment_shader);
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
    lux::engine::resource::Image image("D:/Code/lux-game/playground/render/opengl3/texture/miku.png");
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

    glUniform1i(glGetUniformLocation(shader_program.rawProgramObject(), "ourTexture"), 0);

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

    glfwSwapInterval(1);
    while(!glfwWindowShouldClose(window))
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

        Eigen::Matrix4f view_transform = 
            lux::engine::core::viewTransform({0, 0, 400.0f}, Eigen::Matrix3f::Identity());

        float aspect = global_width / (float)global_height;
        Eigen::Matrix4f projection_transform =  // can be also implement by frustumMatrix
            lux::engine::core::perspectiveMatrix(EIGEN_PI / 4, aspect, 0.1f, (float)10000);

        float current_time = (float)glfwGetTime();

        for(const auto& position : cubePositions)
        {
            Eigen::Affine3f model_transform = 
            lux::engine::core::createTransform(
                Eigen::Vector3f{current_time,current_time,current_time}, 
                {position[0], position[1], position[2]}
            );

            auto mvp = projection_transform * view_transform * model_transform;

            glUniformMatrix4fv(
                glGetUniformLocation(shader_program.rawProgramObject(), "mvp"), 1, GL_FALSE,  mvp.data()
            );

            glDrawArrays(GL_TRIANGLES, 0, 36);
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    return 0;
}

#include <lux-engine/platform/cxx/SubProgram.hpp>
RegistFunctionSubProgram(__main, "coordinate_system")


/*
float aspect = global_width / (float)global_height;
float near  = 1.0f;
float far   = 10000.0f;
lux::engine::core::ProjectionDescription desc;
desc.left   = -aspect * near;
desc.right  =  aspect * near;
desc.bottom = -near;
desc.top    =  near;
desc.near   =  near;
desc.far    =  far;
Eigen::Matrix4f projection_transform = 
    lux::engine::core::frustumMatrix(desc);
*/
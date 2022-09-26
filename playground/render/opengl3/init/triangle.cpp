#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <Eigen/Eigen>

#include <graphic_api_wrapper/opengl3/VertexBufferObject.hpp>
#include <graphic_api_wrapper/opengl3/ShaderProgram.hpp>

#include <thread>
#include <iostream>

static const char* predifined_vertex_shader =
R"(
#version 330 core
layout (location = 0) in vec3 aPos;
void main()
{
   gl_Position = vec4(aPos.x, aPos.y, aPos.z, 1.0);
}
)";

static const char* predefined_fragment_shader =
R"(
#version 330 core
out vec4 FragColor;

void main()
{
    FragColor = vec4(1.0f, 0.5f, 0.2f, 1.0f);
} 
)";

static void sizeChangedCallback(GLFWwindow* window, int width, int height)
{
    glViewport(0, 0, width, height);
}

static int __main(int argc, char* argv[])
{
    Eigen::Vector3f vertexs[] = {
        {-0.5f, -0.5f,  0.0f},
        {0.5f,  -0.5f,  0.0f},
        {0.0f,   0.5f,  0.0f}
    };

    if(glfwInit()!= GLFW_TRUE)
    {
        std::cout << "GLFW init failed!" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    auto window = glfwCreateWindow(1280, 720, "test", nullptr, nullptr);
    if(!window)
    {
        std::cerr << "Create Window failed!" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSetWindowSizeCallback(window, sizeChangedCallback);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return -1;
    }
    std::cout << glGetString(GL_VERSION) << std::endl;;
    glViewport(0, 0, 1280, 720);

    lux::engine::function::ShaderProgram    shader_program;

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

    GLuint vbo;
    glGenBuffers(1, &vbo);

    unsigned int VAO;
    glGenVertexArrays(1, &VAO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertexs), &vertexs, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector3f), (void*)0);
    glEnableVertexAttribArray(0);

    while(!glfwWindowShouldClose(window))
    {
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        shader_program.use();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 3);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    return 0;
}

#include <lux-engine/platform/cxx/SubProgram.hpp>
RegistFunctionSubProgram(__main, "triangle")

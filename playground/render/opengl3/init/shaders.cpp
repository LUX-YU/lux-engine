#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <Eigen/Eigen>

#include <graphic_api_wrapper/opengl3/VertexBufferObject.hpp>
#include <graphic_api_wrapper/opengl3/ShaderProgram.hpp>

static const char* predifined_vertex_shader =
R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aColor;

out vec3 ourColor;

void main()
{
   gl_Position  = vec4(aPos, 1.0);
   ourColor     = aColor;
}

)";

static const char* predefined_fragment_shader =
R"(
#version 330 core
out vec4 FragColor;
in  vec3 ourColor;

void main()
{
    FragColor = vec4(ourColor, 1.0);
} 
)";

static void sizeChangedCallback(GLFWwindow* window, int width, int height)
{
    glViewport(0, 0, width, height);
}

static int __main(int argc, char* argv[])
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

    auto window = glfwCreateWindow(1280, 720, "shader", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    glfwSetWindowSizeCallback(window, sizeChangedCallback);

    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

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

    using Vector6f = Eigen::Matrix<float, 6, 1>;
    static Vector6f vertexs[3]{
        {-0.5,  -0.5,   0,  1.0f,   0,      0},
        {0,      0.5,   0,  0,      1.0f,   0.0},
        {0.5,   -0.5,   0,  0,      0,      1.0f}
    };

    GLuint vbo;
    glGenBuffers(1, &vbo);

    GLuint vao;
    glGenVertexArrays(1, &vao);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertexs), &vertexs, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vector6f), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vector6f), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    shader_program.use();

    int nrAttributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &nrAttributes);
    std::cout << "Maximum nr of vertex attributes supported: " << nrAttributes << std::endl;

    while(!glfwWindowShouldClose(window))
    {
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

        float glfwTime   = static_cast<float>(glfwGetTime());
        float greenValue = (sin(glfwTime / 2.0f)) + 0.5f;
        int vertexColorLocation = shader_program.uniformFindLocationUnsafe("triangleColor");
        shader_program.use();
        glUniform4f(vertexColorLocation, 0.0f, greenValue, 0.0f, 1.0f);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 3);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    return 0;
}

#include <lux-engine/platform/cxx/SubProgram.hpp>
RegistFunctionSubProgram(__main, "shader")

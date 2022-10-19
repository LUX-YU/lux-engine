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
layout (location = 1) in vec3 aNormal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out     vec3 Normal;
out     vec3 FragPos;

void main()
{
    gl_Position = projection * view * model * vec4(aPos, 1.0f);
    FragPos = vec3(model * vec4(aPos, 1.0f));
    Normal = aNormal;
}
)";

static const char* predefined_fragment_shader =
R"(
#version 330 core
struct Material
{
    vec3    ambient;
    vec3    diffuse;
    vec3    specular;
    float   shininess;
};

struct Light
{
    vec3 position;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

out vec4 color;

in vec3 Normal;
in vec3 FragPos;
  
uniform vec3 lightPos; 
uniform vec3 lightColor;
uniform vec3 objectColor;
uniform vec3 viewPos;

uniform Material    material;
uniform Light       light;

void main()
{
    // Ambient
    float ambientStrength   = 1.0f;
    float specularStrength  = 0.5f;
    
    // ambient light
    vec3    ambient     = light.ambient * material.ambient;

    // diffuse
    vec3    norm        = normalize(Normal);
    vec3    lightDir    = normalize(lightPos - FragPos);
    float   diff        = max(dot(norm, lightDir), 0.0);
    vec3    diffuse     = light.diffuse * (diff * material.diffuse);

    // specular
    vec3    viewDir     = normalize(viewPos - FragPos);
    vec3    reflectDir  = reflect(-lightDir, norm);
    float   spec        = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);
    vec3    specular    = light.specular * (spec * material.specular);

    color = vec4( ambient + diffuse + specular, 0.0 );
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
            if(!is_success) {
                std::string error_msg;
                shader->getCompileMessage(error_msg);
                std::cerr << "compile failed!" << std::endl; 
                std::cerr << error_msg << std::endl;
                return -1;
            };
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
            if(!is_success) {
                std::string error_msg;
                shader->getCompileMessage(error_msg);
                std::cerr << "compile failed!" << std::endl; 
                std::cerr << error_msg << std::endl;
                return -1;
            };
            light_program.attachShader(*shader);
        }
        light_program.link(info);
    }


    GLuint vbo;
    // vbo set
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube_vertex_normal), cube_vertex_normal, GL_STATIC_DRAW);

    GLuint cube_vao;
    glGenVertexArrays(1, &cube_vao);
    glBindVertexArray(cube_vao);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector6f), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector6f), (GLvoid*)(3 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    // light vao
    // glBindBuffer(GL_ARRAY_BUFFER, vbo);
    GLuint light_vao;
    glGenVertexArrays(1, &light_vao);
    glBindVertexArray(light_vao);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector6f), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);

    auto light_position = std::array<float, 3>{120.0f, 100.0f, 200.0f};
    auto light_color    = std::array<float, 3>{1.0f, 1.0f, 1.0f};
    auto cube_color     = std::array<float, 3>{1.0f, 0.5f, 0.31f};
    cube_program.use();

    auto location_object_color  = cube_program.uniformFindLocationUnsafe("objectColor");
    auto location_light_color   = cube_program.uniformFindLocationUnsafe("lightColor");
    auto location_light_position= cube_program.uniformFindLocationUnsafe("lightPos");
    auto location_view_position = cube_program.uniformFindLocationUnsafe("viewPos");

    auto material_attr_ambient  = cube_program.uniformFindLocationUnsafe("material.ambient");
    auto material_attr_diffuse  = cube_program.uniformFindLocationUnsafe("material.diffuse");
    auto material_attr_specular = cube_program.uniformFindLocationUnsafe("material.specular");
    auto material_attr_shininess= cube_program.uniformFindLocationUnsafe("material.shininess");

    auto light_attr_ambient     = cube_program.uniformFindLocationUnsafe("light.ambient");
    auto light_attr_diffuse     = cube_program.uniformFindLocationUnsafe("light.diffuse");
    auto light_attr_specular    = cube_program.uniformFindLocationUnsafe("light.specular");
    auto light_attr_position    = cube_program.uniformFindLocationUnsafe("light.position");

    cube_program.uniformSetVector(material_attr_ambient,    Eigen::Vector3f{1.0f, 0.5f, 0.31f});
    cube_program.uniformSetVector(material_attr_diffuse,    std::array{1.0f, 0.5f, 0.31f});
    cube_program.uniformSetVector(material_attr_specular,   std::array{0.5f, 0.5f, 0.5f});
    cube_program.uniformSetVector(material_attr_shininess,  32.0f);

    float light_ambient_factor = 0.2f;
    float light_diffuse_factor = 0.5f;

    GLint cube_mvp_location[3]{
        cube_program.uniformFindLocationUnsafe("model"),
        cube_program.uniformFindLocationUnsafe("view"),
        cube_program.uniformFindLocationUnsafe("projection")
    };

    light_program.use();
    GLint light_mvp_location[3]{
        light_program.uniformFindLocationUnsafe("model"),
        light_program.uniformFindLocationUnsafe("view"),
        light_program.uniformFindLocationUnsafe("projection")
    };
    
    float deltaTime = 0.0f; // delta time
    float lastFrame = 0.0f; // last frame time

    function::UserControlCamera camera(window);

    window.enableVsync(true);
    glfwSetInputMode((GLFWwindow*)window.lowLayerPointer(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // cube position
    Eigen::Affine3f cube_mode  = core::createTransform(Eigen::Vector3f{0,0,0}, {0,0,0});
    // light position
    Eigen::Affine3f light_mode = core::createTransform(Eigen::Vector3f{0,0,0}, light_position);
    core::createTransform(Eigen::Matrix3f{Eigen::Matrix3f::Identity()}, light_position);
    
    while(!window.shouldClose())
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

        float currentFrame = platform::LuxWindow::timeAfterFirstInitialization();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        camera.setCameraSpeed(200.0f * deltaTime);
        camera.updateViewInLoop();
        
        Eigen::Vector3f lightColor;
        lightColor.x() = (float)sin(currentFrame * 2.0f);
        lightColor.y() = (float)sin(currentFrame * 0.7);
        lightColor.z() = (float)sin(currentFrame * 1.3f);
        Eigen::Vector3f diffuseColor = lightColor * light_diffuse_factor;
        Eigen::Vector3f ambientColor = lightColor * light_ambient_factor;

        Eigen::Matrix4f projection_transform = 
            core::perspectiveMatrix(camera.fov() * EIGEN_PI / 180, global_width / (float)global_height, 0.1f, 50000.0f);

        cube_program.use();
        cube_program.uniformSetMatrix(cube_mvp_location[0], false, cube_mode);
        cube_program.uniformSetMatrix(cube_mvp_location[1], false, camera.viewMatrix());
        cube_program.uniformSetMatrix(cube_mvp_location[2], false, projection_transform);

        cube_program.uniformSetVector(light_attr_ambient,       ambientColor);
        cube_program.uniformSetVector(light_attr_diffuse,       diffuseColor);
        cube_program.uniformSetVector(light_attr_specular,      std::array{1.0f, 1.0f, 1.0f});

        cube_program.uniformSetVector(location_object_color,    cube_color);
        cube_program.uniformSetVector(location_light_color,     light_color);
        cube_program.uniformSetVector(location_light_position,  light_position);
        cube_program.uniformSetVector(location_view_position,   camera.cameraPosition());

        glBindVertexArray(cube_vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        light_program.use();
        light_program.uniformSetMatrix(light_mvp_location[0], false, light_mode);
        light_program.uniformSetMatrix(light_mvp_location[1], false, camera.viewMatrix());
        light_program.uniformSetMatrix(light_mvp_location[2], false, projection_transform);
        glBindVertexArray(light_vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        window.swapBuffer();
        platform::LuxWindow::pollEvents();
    }

    return 0;
}

#include <lux-engine/platform/cxx/SubProgram.hpp>
RegistFunctionSubProgram(__main, "materials")

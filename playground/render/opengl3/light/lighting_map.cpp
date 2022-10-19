#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <functional>

#include <lux-engine/resource/image/Image.hpp>
#include <lux-engine/platform/window/LuxWindow.hpp>
#include <lux-engine/core/math/EigenTools.hpp>
#include <render_helper/CameraHelper.hpp>

#include <graphic_api_wrapper/opengl3/VertexBufferObject.hpp>
#include <graphic_api_wrapper/opengl3/ShaderProgram.hpp>

#include <imgui.h>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "CubeVertex.hpp"
#include "render_context.hpp"

static int global_width  = 1920;
static int global_height = 1080;

static void glad_init()
{
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    int nrAttributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &nrAttributes);
}

static int __main(int argc, char* argv[])
{
    using namespace lux::engine::platform;
    using namespace lux::engine::resource;
    using namespace lux::engine::core;
    using namespace lux::engine::function;
    LuxWindow window(global_width, global_height, "color");
    glad_init();
    
    ShaderProgram cube_program;
    ShaderProgram light_program;

    std::string shader_path[]{
        std::string(opengl_shader_path) + "/vertex_with_normal.vert",
        std::string(opengl_shader_path) + "/light_caster.frag",
        std::string(opengl_shader_path) + "/common_light.frag"
    };
    auto vertex_shader      = GlShader<ShaderType::VERTEX>::loadFromFile(shader_path[0]);
    auto fragment_shader_1  = GlShader<ShaderType::FRAGMENT>::loadFromFile(shader_path[1]);
    auto fragment_shader_2  = GlShader<ShaderType::FRAGMENT>::loadFromFile(shader_path[2]);
    vertex_shader.compile();
    fragment_shader_1.compile();
    fragment_shader_2.compile();

    cube_program.attachShader(vertex_shader);
    cube_program.attachShader(fragment_shader_1);
    cube_program.link();

    light_program.attachShader(vertex_shader);
    light_program.attachShader(fragment_shader_2);
    light_program.link();

    GLuint vbo;
    // vbo set
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube_vertex_normal_texture), cube_vertex_normal_texture, GL_STATIC_DRAW);

    GLuint cube_vao;
    glGenVertexArrays(1, &cube_vao);
    glBindVertexArray(cube_vao);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector8f), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector8f), (GLvoid*)(3 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector8f), (GLvoid*)(6 * sizeof(GLfloat)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    // light vao
    // glBindBuffer(GL_ARRAY_BUFFER, vbo);
    GLuint light_vao;
    glGenVertexArrays(1, &light_vao);
    glBindVertexArray(light_vao);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector8f), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);

    // textures
    unsigned int textures[2];
    std::string  paths[]{
        std::string(texture_path) + "/container2.png",
        std::string(texture_path) + "/container2_specular.png"
    };
    for(uint8_t count = 0; count < 2 ; count ++)
    {
        glGenTextures(1, &textures[count]);
        glBindTexture(GL_TEXTURE_2D, textures[count]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,       GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,       GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,   GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,   GL_NEAREST_MIPMAP_NEAREST);
        Image image(paths[count]);
        if(!image.isEnable())
        {
            std::cout << "Failed to load texture" << std::endl;
            return -1;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    // light and cube attribute
    auto light_position = std::array<float, 3>{120.0f, 100.0f, 200.0f};
    cube_program.use();
    auto location_view_position = cube_program.uniformFindLocationUnsafe("viewPos");

    auto material_attr_diffuse  = cube_program.uniformFindLocationUnsafe("material.diffuse");
    auto material_attr_specular = cube_program.uniformFindLocationUnsafe("material.specular");
    auto material_attr_shininess= cube_program.uniformFindLocationUnsafe("material.shininess");

    auto light_attr_ambient     = cube_program.uniformFindLocationUnsafe("light.ambient");
    auto light_attr_diffuse     = cube_program.uniformFindLocationUnsafe("light.diffuse");
    auto light_attr_specular    = cube_program.uniformFindLocationUnsafe("light.specular");
    auto light_attr_position    = cube_program.uniformFindLocationUnsafe("light.position");

    cube_program.uniformSetVector(light_attr_ambient,           0.2f, 0.2f, 0.2);
    cube_program.uniformSetVector(light_attr_diffuse,           0.5f, 0.5f, 0.5f);
    cube_program.uniformSetVector(light_attr_specular,          1.0f, 1.0f, 1.0f);
    cube_program.uniformSetVector(light_attr_position,          light_position);
    cube_program.uniformSetVector<int>(material_attr_diffuse,   0);
    cube_program.uniformSetVector<int>(material_attr_specular,  1);
    cube_program.uniformSetVector(material_attr_shininess,      64.0f);

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

    UserControlCamera camera(window);
    window.subscribeMouseButtonCallback(
        [&camera](auto&, auto button, auto action, auto mods)
        {
            if(button == MouseButton::MOUSE_BUTTON_LEFT)
            {
                if(action == KeyState::PRESS)
                {
                    camera.enableMouseControl();
                }
                else if(action == KeyState::RELEASE)
                {
                    camera.disableMouseControl();
                }
            }
        }
    );

    window.enableVsync(false);

    // cube position
    Eigen::Vector3f cube_position{0,0,0};
    // light position
    Eigen::Affine3f light_model = createTransform(Eigen::Vector3f{0,0,0}, light_position);
    createTransform(Eigen::Matrix3f{Eigen::Matrix3f::Identity()}, light_position);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window.lowLayerPointer(), true);
    ImGui_ImplOpenGL3_Init("#version 130");
    
    float camera_speed = 200.0f;
    while(!window.shouldClose())
    {
        LuxWindow::pollEvents();
        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        float currentFrame = LuxWindow::timeAfterFirstInitialization();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        {
            ImGui::Begin("parameter panel!");
            ImGui::SliderFloat("camera speed", &camera_speed, 0.0f, 2000.0f);
            ImGui::SliderFloat3("cube position", (float*)&cube_position, -1000, 1000, "%.3f", 0);
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
            ImGui::End();
        }

        camera.setCameraSpeed(camera_speed * deltaTime);
        camera.updateViewInLoop();

        Eigen::Matrix4f projection_transform = 
            perspectiveMatrix(camera.fov() * EIGEN_PI / 180, global_width / (float)global_height, 0.1f, 50000.0f);

        cube_program.use();
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, textures[1]);

        float time = (float)glfwGetTime();
        auto cube_model  = createTransform(
            Eigen::Vector3f{time,time,time}, 
            cube_position
        );
        cube_program.uniformSetMatrix(cube_mvp_location[0], false, cube_model);
        cube_program.uniformSetMatrix(cube_mvp_location[1], false, camera.viewMatrix());
        cube_program.uniformSetMatrix(cube_mvp_location[2], false, projection_transform);
        cube_program.uniformSetVector(location_view_position,   camera.cameraPosition());
        
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

        glBindVertexArray(cube_vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        light_program.use();
        light_program.uniformSetMatrix(light_mvp_location[0], false, light_model);
        light_program.uniformSetMatrix(light_mvp_location[1], false, camera.viewMatrix());
        light_program.uniformSetMatrix(light_mvp_location[2], false, projection_transform);

        ImGui::Render();
        glBindVertexArray(light_vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        window.swapBuffer();
    }

    return 0;
}

#include <lux-engine/platform/cxx/SubProgram.hpp>
RegistFunctionSubProgram(__main, "lighting_map")

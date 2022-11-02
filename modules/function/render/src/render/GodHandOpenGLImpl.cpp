#include "GodHandOpenGLImpl.hpp"

namespace lux::engine::function
{
    static const char* default_vertex=
R"(#version 330 core
layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 texCoords;

out vec2 TexCoords;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{
    gl_Position = projection * view * model * vec4(position, 1.0f);
    TexCoords = texCoords;
})";

    GodHandOpenGLImpl::GodHandOpenGLImpl()
    {

    }

    void GodHandOpenGLImpl::drawModel(const LuxModel& model, const Position& position)
    {
        
    } 
}

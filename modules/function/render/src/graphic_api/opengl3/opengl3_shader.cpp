#include "lux-engine/function/graphic_api/opengl3/Shader.hpp"
#include <iostream>
#include <optional>

namespace lux::engine::function
{   
    GLuint GlShaderBase::_forward_convert(ShaderType type)
    {
        switch(type)
        {
            case ShaderType::VERTEX:
                return GL_VERTEX_SHADER;
            case ShaderType::FRAGMENT:
                return GL_FRAGMENT_SHADER;
        };
        return GL_VERTEX_SHADER;
    };

    ShaderType GlShaderBase::_inverse_convert(GLuint type)
    {
        switch(type)
        {
            case GL_VERTEX_SHADER:
                return ShaderType::VERTEX;
            case GL_FRAGMENT_SHADER:
                return ShaderType::FRAGMENT;
        };
        return ShaderType::VERTEX;
    }
}
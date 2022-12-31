#include "lux/opengl3/Shader.hpp"
#include <iostream>
#include <optional>

namespace lux::gapiwrap::opengl
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
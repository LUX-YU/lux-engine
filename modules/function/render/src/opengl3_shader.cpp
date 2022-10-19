#include "graphic_api_wrapper/opengl3/Shader.hpp"
#include <iostream>
#include <optional>

namespace lux::engine::function
{
    
    static GLuint _forward_convert(ShaderType type)
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

    static ShaderType _inverse_convert(GLuint type)
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

    void GlShaderBase::createShader(ShaderType type)
    {
        _shader_object = glCreateShader(_forward_convert(type));
    }

    GlShaderBase::~GlShaderBase() = default;
    // {
    //     GLint status;
    //     glGetShaderiv(_shader_object, GL_DELETE_STATUS, &status);
    //     if(status == GL_FALSE)
    //     {
    //         glDeleteShader(_shader_object);
    //     }
    // }

    bool GlShaderBase::isReleased()
    {
        GLint status;
        glGetShaderiv(_shader_object, GL_DELETE_STATUS, &status);
        return status != GL_FALSE;
    }

    bool GlShaderBase::released()
    {
        return isReleased() ? glDeleteShader(_shader_object) , true : false;
    }

    void GlShaderBase::shaderSource(const std::string& source)
    {
        auto _source_pointer = source.c_str();
        glShaderSource(_shader_object, 1, &_source_pointer, nullptr);
    }

    void GlShaderBase::shaderSource(const char *source)
    {
        glShaderSource(_shader_object, 1, &source, nullptr);
    }

    bool GlShaderBase::compile(std::string &info)
    {
        int success;
        glCompileShader(_shader_object);
        glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &success);

        if (!success)
        {
            getCompileMessage(info);
            return false;
        }
        return true;
    }

    bool GlShaderBase::compile()
    {
        GLint success;
        glCompileShader(_shader_object);
        glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &success);
        return success;
    }

    bool GlShaderBase::isCompiled()
    {
        GLint status;
        glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &status);
        return status == GL_TRUE;
    }

    void GlShaderBase::getCompileMessage(std::string &info)
    {
        GLint info_lenght;
        glGetShaderiv(_shader_object, GL_INFO_LOG_LENGTH, &info_lenght);
        info.resize(info_lenght);
        // std::string is guaranteed to be contiguous since C++11
        glGetShaderInfoLog(_shader_object, info_lenght, nullptr, info.data());
    }

    ShaderType GlShaderBase::shaderType()
    {
        GLint type;
        glGetShaderiv(_shader_object, GL_SHADER_TYPE, &type);
        return _inverse_convert(static_cast<GLuint>(type));
    }
}
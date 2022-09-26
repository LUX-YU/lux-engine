#include "graphic_api_wrapper/opengl3/ShaderProgram.hpp"

namespace lux::engine::function
{
    ShaderProgram::ShaderProgram()
    {
        _shader_program_object = glCreateProgram();
    }

    ShaderProgram::~ShaderProgram()
    {
        glDeleteProgram(_shader_program_object);
    }

    ShaderProgram &ShaderProgram::attachShader(const GlShader &shader)
    {
        glAttachShader(_shader_program_object, shader._shader_object);
        return *this;
    }

    bool ShaderProgram::link(std::string &info)
    {
        GLint success;
        glLinkProgram(_shader_program_object);
        glGetProgramiv(_shader_program_object, GL_LINK_STATUS, &success);
        if (!success)
        {
            char info_buffer[512];
            glGetProgramInfoLog(_shader_program_object, 512, NULL, info_buffer);
            info = info_buffer;
            return false;
        }
        return true;
    }

    bool ShaderProgram::link()
    {
        GLint success;
        glLinkProgram(_shader_program_object);
        glGetProgramiv(_shader_program_object, GL_LINK_STATUS, &success);
        return success;
    }

    void ShaderProgram::use()
    {
        glUseProgram(_shader_program_object);
    }

    void ShaderProgram::getLinkMessage(std::string &info)
    {
        char info_buffer[512];
        glGetProgramInfoLog(_shader_program_object, 512, nullptr, info_buffer);
        info = info_buffer;
    }

    GLuint ShaderProgram::rawProgramObject()
    {
        return _shader_program_object;
    }
}
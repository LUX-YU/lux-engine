#include "graphic_api_wrapper/opengl3/ShaderProgram.hpp"
#include <filesystem>

namespace lux::engine::function
{
    bool is_file_exists(const std::string& file_path)
    {
        return std::filesystem::exists(file_path) && std::filesystem::is_regular_file(file_path);
    }

    ShaderProgram::ShaderProgram()
    {
        _shader_program_object = glCreateProgram();
    }

    ShaderProgram::~ShaderProgram() = default;

    ShaderProgram &ShaderProgram::attachShader(const GlShaderBase &shader)
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
            getLinkMessage(info);
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

    void ShaderProgram::release()
    {
        glDeleteProgram(_shader_program_object);
    }

    bool ShaderProgram::operator==(ShaderProgram other)
    {
        return _shader_program_object == other._shader_program_object;
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
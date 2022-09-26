#include "graphic_api_wrapper/opengl3/Shader.hpp"
#include <iostream>

namespace lux::engine::function
{
    GlShader::GlShader(GLenum type, std::string source)
    {
        std::cout << "one" << std::endl;
        _shader_object = glCreateShader(type);
        shaderSource(std::move(source));
    }

    GlShader::GlShader(GLenum type, const char *const *source)
    {
        std::cout << "one" << std::endl;
        _shader_object = glCreateShader(type);
        shaderSource(source);
    }

    GlShader::~GlShader()
    {
        std::cout << "two`" << std::endl;
        glDeleteShader(_shader_object);
    }

    void GlShader::shaderSource(const std::string& source)
    {
        auto _source_pointer = source.c_str();
        glShaderSource(_shader_object, 1, &_source_pointer, nullptr);
    }

    void GlShader::shaderSource(const char *const *source)
    {
        glShaderSource(_shader_object, 1, source, nullptr);
    }

    bool GlShader::compile(std::string &info)
    {
        int success;
        glCompileShader(_shader_object);
        glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &success);

        if (!success)
        {
            char info_buffer[512];
            glGetShaderInfoLog(_shader_object, 512, nullptr, info_buffer);
            info = info_buffer;
            return false;
        }
        return true;
    }

    bool GlShader::compile()
    {
        GLint success;
        glCompileShader(_shader_object);
        glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &success);
        return success;
    }

    void GlShader::getCompileMessage(std::string &info)
    {
        char info_buffer[512];
        glGetShaderInfoLog(_shader_object, 512, nullptr, info_buffer);
        info = info_buffer;
    }

    GlVertexShader::GlVertexShader(const std::string& source)
        : GlShader(GL_VERTEX_SHADER, source) {}

    GlVertexShader::GlVertexShader(const char *const *source)
        : GlShader(GL_VERTEX_SHADER, source) {}

    GlFragmentShader::GlFragmentShader(const std::string& source)
        : GlShader(GL_FRAGMENT_SHADER, source) {}

    GlFragmentShader::GlFragmentShader(const char *const *source)
        : GlShader(GL_FRAGMENT_SHADER, source) {}
}
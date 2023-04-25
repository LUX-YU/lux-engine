#pragma once
#include <glad/glad.h>
#include <string>
#include <string_view>

#include <fstream>
#include <streambuf>

namespace lux::gapi::opengl
{
    enum class ShaderType
    {
        VERTEX      = GL_VERTEX_SHADER,
        FRAGMENT    = GL_FRAGMENT_SHADER,
        GEOMETRY    = GL_GEOMETRY_SHADER
    };

    class GlShaderBase
    {
    protected:
        explicit GlShaderBase(ShaderType type)
        {
            _shader_object = glCreateShader(static_cast<GLenum>(type));
        }

    public:

        GlShaderBase(GlShaderBase&) = delete;
        GlShaderBase& operator=(GlShaderBase&) = delete;

        GlShaderBase(GlShaderBase&& other) noexcept
        {
            this->_shader_object = other._shader_object;
            other._shader_object = 0;
        }

        GlShaderBase& operator=(GlShaderBase&& other)
        {
            release();
            this->_shader_object = other._shader_object;
            other._shader_object = 0;
            return *this;
        }

        ~GlShaderBase()
        {
            release();
        }

        GLuint rawObject()
        {
            return _shader_object;
        }

        bool operator==(GlShaderBase& other) const
        {
            return other._shader_object == _shader_object;
        }

        bool compile(const char* source)
        {
            int success;
            // this is ok to get parameter address
            // because the source will be compile in this function

            // https://registry.khronos.org/OpenGL-Refpages/gl4/html/glShaderSource.xhtml
            glShaderSource(_shader_object, 1, &source, nullptr);

            glCompileShader(_shader_object);
            glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &success);

            return success == GL_TRUE;
        }

        bool compile(const std::string& source)
        {
            return compile(source.c_str());
        }

        [[nodiscard]] bool isCompiled() const
        {
            GLint status;
            glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &status);
            return status == GL_TRUE;
        }

        [[nodiscard]] bool isReleased() const
        {
            return _shader_object == 0;
        }

        void release()
        {
            if(_shader_object != 0)
            {
                glDeleteShader(_shader_object);
                _shader_object = 0;
            }
        }

    private:
        GLuint _shader_object;
    };

    template <ShaderType Shader_TYPE>
    class TShader : public GlShaderBase
    {
    public:
        TShader() : GlShaderBase(Shader_TYPE) {}
    };

    using VertexShader   = TShader<ShaderType::VERTEX>;
    using FragmentShader = TShader<ShaderType::FRAGMENT>;
    using GeometryShader = TShader<ShaderType::GEOMETRY>;
}

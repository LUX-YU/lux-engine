#pragma once
#include <lux-engine/platform/system/visibility_control.h>

#include <glad/glad.h>
#include <string>
#include <string_view>

#include <fstream>
#include <sstream>
#include <streambuf>

namespace lux::engine::function
{
    enum class ShaderType
    {
        VERTEX,
        FRAGMENT,
        GEOMETRY
    };

    template <ShaderType>
    class GlShader;

    class GlShaderBase
    {
        template <class T>
        friend class GlShaderTypeHelper;

    protected:
        LUX_EXPORT static GLuint _forward_convert(ShaderType type);
        LUX_EXPORT static ShaderType _inverse_convert(GLuint type);

        void createShader(ShaderType type)
        {
            _shader_object = glCreateShader(_forward_convert(type));
        }

        void shaderSource(const std::string &source)
        {
            auto _source_pointer = source.c_str();
            glShaderSource(_shader_object, 1, &_source_pointer, nullptr);
        }

        void shaderSource(const char *source)
        {
            glShaderSource(_shader_object, 1, &source, nullptr);
        }

    public:
        template <typename _CType>
        GlShaderBase(ShaderType type, _CType &&source)
        {
            createShader(type);
            shaderSource(std::forward<_CType>(source));
        }

        bool operator==(GlShaderBase other)
        {
            return other._shader_object == _shader_object;
        }

        ~GlShaderBase() = default;

        template <ShaderType _Shader_Type>
        GlShader<_Shader_Type> shaderCast()
        {

            return static_cast<GlShader<_Shader_Type>>(*this);
        }

        bool compile(std::string &info)
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

        bool GlShaderBase::isReleased()
        {
            GLint status;
            glGetShaderiv(_shader_object, GL_DELETE_STATUS, &status);
            return status != GL_FALSE;
        }

        bool GlShaderBase::released()
        {
            return isReleased() ? glDeleteShader(_shader_object), true : false;
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

    private:
        friend class ShaderProgram;
        GLuint _shader_object;
    };

    // TODO move to platform layer
    bool is_file_exists(const std::string &file_path);

    template <ShaderType _Shader_TYPE>
    class GlShader : public GlShaderBase
    {
    public:
        template <typename _CType>
        GlShader(_CType &&shader_src_code)
            : GlShaderBase(_Shader_TYPE, std::forward<_CType>(shader_src_code)) {}

        template <typename _CType>
        static GlShader<_Shader_TYPE> loadFromFile(_CType &&path)
        {
            std::ifstream t(std::forward<_CType>(path));
            return GlShader<_Shader_TYPE>(
                std::string(
                    std::istreambuf_iterator<char>(t),
                    std::istreambuf_iterator<char>()));
        }
    };
}

#pragma once
#include <glad/glad.h>
#include <string>
#include <string_view>

#include <fstream>
#include <streambuf>

namespace lux::engine::function
{
    class GlShader
    {
        template<class T> friend class GlShaderTypeHelper;
    protected:
        GlShader(GLenum type, std::string source);

        GlShader(GLenum type, const char* const* source);

    public:
        ~GlShader();

        void shaderSource(const std::string& source);

        void shaderSource(const char* const* source);

        bool compile(std::string& info);

        bool compile();

        void getCompileMessage(std::string& info);

        GLenum shaderType();
    
    private:
        friend class ShaderProgram;
        GLuint      _shader_object;
    };

    template<class T>
    class GlShaderTypeHelper
    {
    public:
        static T loadFrom(std::string path)
        {
            std::ifstream t(std::move(path));
            return T(std::string(
                std::istreambuf_iterator<char>(t),
                std::istreambuf_iterator<char>()
                )
            );
        }
    };

    class GlVertexShader : public GlShader, public GlShaderTypeHelper<GlVertexShader>
    {
    public:
        explicit GlVertexShader(const std::string& source);

        explicit GlVertexShader(const char* const* source);
    };

    class GlFragmentShader : public GlShader, public GlShaderTypeHelper<GlFragmentShader>
    {
    public:
        explicit GlFragmentShader(const std::string& source);

        explicit GlFragmentShader(const char* const* source);
    };
}

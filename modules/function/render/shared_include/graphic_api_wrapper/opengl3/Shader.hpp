#pragma once
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

    template<ShaderType> class GlShader;

    class GlShaderBase
    {
        template<class T> friend class GlShaderTypeHelper;
    protected:
        void createShader(ShaderType type);

        void shaderSource(const std::string& source);

        void shaderSource(const char* source);

    public:
        template<typename _CType>
        GlShaderBase(ShaderType type, _CType&& source)
        {
            createShader(type);
            shaderSource(std::forward<_CType>(source));
        }

        bool operator==(GlShaderBase other)
        {
            return other._shader_object == _shader_object;
        }

        static GlShaderBase loadFromFile(ShaderType type, const char* path);

        ~GlShaderBase();

        template<ShaderType _Shader_Type>
        GlShader<_Shader_Type> shaderCast()
        {
            
            return static_cast<GlShader<_Shader_Type>>(*this);
        }

        bool compile(std::string& info);

        bool compile();

        bool isCompiled();

        bool isReleased();

        bool released();

        void getCompileMessage(std::string& info);

        ShaderType shaderType();
    
    private:
        friend class ShaderProgram;
        GLuint      _shader_object;
    };

    // TODO move to platform layer
    bool is_file_exists(const std::string& file_path);

    template<ShaderType _Shader_TYPE>
    class GlShader : public GlShaderBase
    {
    public:
        template<typename _CType>
        GlShader(_CType&& shader_src_code)
        : GlShaderBase(_Shader_TYPE, std::forward<_CType>(shader_src_code)){}

        template<typename _CType>
        static GlShader<_Shader_TYPE> loadFromFile(_CType&& path)
        {
            std::ifstream t(std::forward<_CType>(path));
            return GlShader<_Shader_TYPE>(
                std::string(
                    std::istreambuf_iterator<char>(t),
                    std::istreambuf_iterator<char>()
                )
            );
        }
    };
}

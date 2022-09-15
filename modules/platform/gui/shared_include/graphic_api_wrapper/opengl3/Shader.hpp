#pragma once
#include <glad/glad.h>
#include <array>
#include <string>
#include <string_view>

namespace lux::engine::platform
{
    class GlShader
    {
    public:
        GlShader(GLenum type)
        {
            _shader_object  = glCreateShader(type);
        }

        GlShader(GLenum type, std::string source)
        {
            _shader_object  = glCreateShader(type);
            shaderSource(std::move(source));
        }

        GlShader(GLenum type, const char* const* source)
        {
            _shader_object  = glCreateShader(type);
            shaderSource(source);
        }

        ~GlShader()
        {
            glDeleteShader(_shader_object);
        }

        void shaderSource(std::string source)
        {
            _source = std::move(source);
            _source_pointer = _source.c_str();
            glShaderSource(_shader_object, 1, &_source_pointer, nullptr);
        }

        void shaderSource(const char* const* source)
        {
            _source_pointer = *source;
            glShaderSource(_shader_object, 1, &_source_pointer, nullptr);
        }

        bool haveSource() 
        {
            return !_source.empty() || !_source_pointer;
        }

        bool compile(std::string& info)
        {
            int success;
            glCompileShader(_shader_object);
            glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &success);

            if(!success)
            {
                char info_buffer[512];
                glGetShaderInfoLog(_shader_object, 512, nullptr, info_buffer);
                info = info_buffer;
                return false;
            }
            return true;
        }

        bool compile()
        {
            GLint success;
            glCompileShader(_shader_object);
            glGetShaderiv(_shader_object, GL_COMPILE_STATUS, &success);
            return success;
        }

        void getCompileMessage(std::string& info)
        {
            char info_buffer[512];
            glGetShaderInfoLog(_shader_object, 512, nullptr, info_buffer);
            info = info_buffer;
        }
    
    private:
        friend class ShaderProgram;

        GLuint      _shader_object;
        std::string _source;
        const char* _source_pointer{nullptr};
    };

    class GlVertexShader : public GlShader
    {
    public:
        GlVertexShader()
        : GlShader(GL_VERTEX_SHADER){}

        GlVertexShader(std::string source)
        : GlShader(GL_VERTEX_SHADER, std::move(source)){}

        GlVertexShader(const char* const* source)
        : GlShader(GL_VERTEX_SHADER, source){}
    };

    class GlFragmentShader : public GlShader
    {
    public:
        GlFragmentShader()
        : GlShader(GL_FRAGMENT_SHADER){}

        GlFragmentShader(std::string source)
        : GlShader(GL_FRAGMENT_SHADER, std::move(source)){}

        GlFragmentShader(const char* const* source)
        : GlShader(GL_FRAGMENT_SHADER, source){}
    };

    template<class T, int _LEN> struct GLVectorUniformMap;
    #define GL_VECTOR_MAP_HELPER(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV, TYPE)\
    template<> struct GLVectorUniformMap<TYPE, LEN>{\
        constexpr static auto& method  = GL_UNIFORM_FUNC;\
        constexpr static auto& vmethod = GL_UNIFORM_FUNCV;\
        using value_type = TYPE;\
    };
    #define GL_VECTOR_MAP_HELPER_FLOAT(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV)  GL_VECTOR_MAP_HELPER(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV, float)
    #define GL_VECTOR_MAP_HELPER_INT(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV)    GL_VECTOR_MAP_HELPER(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV, int)
    #define GL_VECTOR_MAP_HELPER_UINT(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV)   GL_VECTOR_MAP_HELPER(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV, unsigned int)

    GL_VECTOR_MAP_HELPER_FLOAT(1,   glUniform1f,  glUniform1fv)  GL_VECTOR_MAP_HELPER_FLOAT(2,  glUniform2f,  glUniform2fv)
    GL_VECTOR_MAP_HELPER_FLOAT(3,   glUniform3f,  glUniform3fv)  GL_VECTOR_MAP_HELPER_FLOAT(4,  glUniform4f,  glUniform4fv)
    GL_VECTOR_MAP_HELPER_INT  (1,   glUniform1i,  glUniform1iv)  GL_VECTOR_MAP_HELPER_INT  (2,  glUniform2i,  glUniform2iv)
    GL_VECTOR_MAP_HELPER_INT  (3,   glUniform3i,  glUniform3iv)  GL_VECTOR_MAP_HELPER_INT  (4,  glUniform4i,  glUniform4iv)
    GL_VECTOR_MAP_HELPER_UINT (1,   glUniform1ui, glUniform1uiv) GL_VECTOR_MAP_HELPER_UINT (2,  glUniform2ui, glUniform2uiv)
    GL_VECTOR_MAP_HELPER_UINT (3,   glUniform3ui, glUniform3uiv) GL_VECTOR_MAP_HELPER_UINT (4,  glUniform4ui, glUniform4uiv)

    enum GL_UNI_MATRIX_TYPE
    {
        UNI_MATRIX_2FV,     UNI_MATRIX_3FV, UNI_MATRIX_4FV,
        UNI_MATRIX_2X3FV,   UNI_MATRIX_3X2FV,
        UNI_MATRIX_2X4FV,   UNI_MATRIX_4X2FV,
        UNI_MATRIX_3X4FV,   UNI_MATRIX_4X3FV
    };

    template<int row, int col> struct GLMatrixUniformMap;
    #define GL_MATRIX_MAP_HELPER(_ROW, _COL, GL_UNIFORM_FUNCV)\
    template<> struct GLMatrixUniformMap<_ROW, _COL>{\
        constexpr static auto& vmethod = GL_UNIFORM_FUNCV;\
        using value_type = float;\
    };
    GL_MATRIX_MAP_HELPER(2,2, glUniformMatrix2fv)    GL_MATRIX_MAP_HELPER(3,3,  glUniformMatrix3fv)
    GL_MATRIX_MAP_HELPER(4,4, glUniformMatrix4fv)    GL_MATRIX_MAP_HELPER(2,3,  glUniformMatrix2x3fv)
    GL_MATRIX_MAP_HELPER(3,2, glUniformMatrix3x2fv)  GL_MATRIX_MAP_HELPER(2,4,  glUniformMatrix2x4fv)
    GL_MATRIX_MAP_HELPER(4,2, glUniformMatrix4x2fv)  GL_MATRIX_MAP_HELPER(3,4,  glUniformMatrix3x4fv)
    GL_MATRIX_MAP_HELPER(4,3, glUniformMatrix4x3fv)

    class ShaderProgram
    {
    public:
        ShaderProgram()
        {
            _shader_program_object = glCreateProgram();
        }

        ~ShaderProgram()
        {
            glDeleteProgram(_shader_program_object);
        }
    
        ShaderProgram& attachShader(const GlShader& shader)
        {
            glAttachShader(_shader_program_object, shader._shader_object);
            return *this;
        }

        bool link(std::string& info)
        {
            GLint success;
            glLinkProgram(_shader_program_object);
            glGetProgramiv(_shader_program_object, GL_LINK_STATUS, &success);
            if(!success)
            {
                char info_buffer[512];
                glGetProgramInfoLog(_shader_program_object, 512, NULL, info_buffer);
                info = info_buffer;
                return false;
            }
            return true;
        }

        bool link()
        {
            GLint success;
            glLinkProgram(_shader_program_object);
            glGetProgramiv(_shader_program_object, GL_LINK_STATUS, &success);
            return success;
        }

        void use()
        {
            glUseProgram(_shader_program_object);
        }

        void getLinkMessage(std::string& info)
        {
            char info_buffer[512];
            glGetProgramInfoLog(_shader_program_object, 512, nullptr, info_buffer);
            info = info_buffer;
        }

        enum class SetUniformValueEnum
        {
            SUCCESS,
            INVALID_VALUE,
            INVALID_OPERATION
        };

        static inline SetUniformValueEnum location_judge(GLint location)
        {
            if(location == GL_INVALID_VALUE) return SetUniformValueEnum::INVALID_VALUE;
            else if(location == GL_INVALID_OPERATION) return SetUniformValueEnum::INVALID_OPERATION;
            return SetUniformValueEnum::SUCCESS;
        }

        template<class T> inline SetUniformValueEnum uniformFindLocation(T, GLint& location);
        template<> inline SetUniformValueEnum uniformFindLocation<const std::string&>(const std::string& name, GLint& location)
        {
            location = glGetUniformLocation(_shader_program_object, name.c_str());
            return location_judge(location);
        }
        template<> inline SetUniformValueEnum uniformFindLocation<const char*>(const char* name, GLint& location)
        {
            location = glGetUniformLocation(_shader_program_object, name);
            return location_judge(location);
        }

        /**
         * @brief get uniform location without return value check
         * 
         * @tparam T 
         * @return GLint 
         */
        template<class T> inline GLint uniformFindLocationUnsafe(T);
        template<> GLint uniformFindLocationUnsafe<const std::string&>(const std::string& name)
        {
            return glGetUniformLocation(_shader_program_object, name.c_str());
        }
        template<> GLint uniformFindLocationUnsafe<const char*>(const char* name)
        {
            return glGetUniformLocation(_shader_program_object, name);
        }
        template<> GLint uniformFindLocationUnsafe<std::string_view>(std::string_view name)
        {
            return glGetUniformLocation(_shader_program_object, name.data());
        }
        template<> inline SetUniformValueEnum uniformFindLocation<std::string_view>(std::string_view name, GLint& location)
        {
            location = glGetUniformLocation(_shader_program_object, name.data());
            return location_judge(location);
        }

        template<class T = float> inline void uniformSetVector(GLint location, const T value)
        {
            GLVectorUniformMap<T, 1>::method(location, value);
        }
        template<class T = float> inline void uniformSetVector(GLint location, const T value1, const T value2)
        {
            GLVectorUniformMap<T, 2>::method(location, value1, value2);
        }
        template<class T = float> inline void uniformSetVector(GLint location, const T value1, const T value2, const T value3)
        {
            GLVectorUniformMap<T, 3>::method(location, value1, value2, value3);
        }
        template<class T = float> inline void uniformSetVector(GLint location, const T value1, const T value2, const T value3, const T value4)
        {
            GLVectorUniformMap<T, 3>::method(location, value1, value2, value3, value4);
        }
        // array support
        template<class T = float> inline void uniformSetVector(GLint location, const std::array<T, 2>& value)
        {
            GLVectorUniformMap<T, 2>::vmethod(location, 1, value.data());
        }
        template<class T = float> inline void uniformSetVector(GLint location, const std::array<T, 3>& value)
        {
            GLVectorUniformMap<T, 3>::vmethod(location, 1, value.data());
        }
        template<class T = float> inline void uniformSetVector(GLint location, const std::array<T, 4>& value)
        {
            GLVectorUniformMap<T, LEN>::vmethod(location, 1, vector.data());
        }
        template<class T = float, int LEN> inline void uniformSetVectorX(GLint location, const std::array<T, LEN>& value)
        {
            GLVectorUniformMap<T, LEN>::vmethod(location, 1, vector.data());
        }
        // eigen support
        template<class T = float> inline void uniformSetVector(GLint location, const Eigen::Matrix<T, 2, 1>& vector)
        {
            GLVectorUniformMap<T, 2>::vmethod(location, 1, vector.data());
        }
        template<class T = float> inline void uniformSetVector(GLint location, const Eigen::Matrix<T, 3, 1>& vector)
        {
            GLVectorUniformMap<T, 3>::vmethod(location, 1, vector.data());
        }
        template<class T = float> inline void uniformSetVector(GLint location, const Eigen::Matrix<T, 4, 1>& vector)
        {
            GLVectorUniformMap<T, 4>::vmethod(location, 1, vector.data());
        }
        template<class T = float, int LEN> inline void uniformSetVectorX(GLint location, const Eigen::Matrix<T, LEN, 1>& vector)
        {
            GLVectorUniformMap<T, LEN>::vmethod(location, 1, vector.data());
        }
        // any type
        template<class T = float, int LEN> inline void uniformSetVectorX(GLint location, T* value)
        {
            GLVectorUniformMap<T, LEN>::vmethod(location, 1, value);
        }

        // unsafae uniform variable set
        template<class T = float, class STRING_TYPE, class... Values> void uniformSetVectorUnsafe(STRING_TYPE&& name, Values&&... values)
        {
            auto location = uniformFindLocationUnsafe(std::forward<STRING_TYPE>(name));
            uniformSetVector<T>(location, std::forward<Values>(values)...);
        }

        template<int ROW, int COL> inline void uniformSetMatrix(GLint location, bool transpose, float* data)
        {
            GLMatrixUniformMap<ROW, COL>::vmethod(location, 1, transpose ? GL_TRUE : GL_FALSE, data);
        }
        template<int ROW, int COL> inline void uniformSetMatrix(GLint location, bool transpose, const Eigen::Matrix<float, ROW, COL>& matrix)
        {
            GLMatrixUniformMap<ROW, COL>::vmethod(location, 1, transpose ? GL_TRUE : GL_FALSE, matrix.data());
        }
        
        GLuint rawProgramObject()
        {
            return _shader_program_object;
        }

    private:
        GLuint _shader_program_object;
    };
}
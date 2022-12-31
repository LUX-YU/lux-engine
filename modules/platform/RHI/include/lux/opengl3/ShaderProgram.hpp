#pragma once
#include "Shader.hpp"
#include "ShaderUniformType.hpp"
#include <Eigen/Eigen>
#include <array>
#include <lux/system/visibility_control.h>

namespace lux::gapiwrap::opengl
{
    class ShaderProgram
    {
    private:
        template<class T, class...>
        struct are_same_basic_type : std::true_type{};

        template<class T, class U, class... TT>
        struct are_same_basic_type<T, U, TT...>
            : std::integral_constant<
        bool, (std::is_same_v<T,U> || std::is_convertible_v<T, U>) 
                && !std::is_class_v<U> && are_same_basic_type<T, TT...>::value>{};

    public:
        ShaderProgram()
        {
            _shader_program_object = glCreateProgram();
        }

        ~ShaderProgram() = default;
    
        inline ShaderProgram& attachShader(const GlShaderBase& shader)
        {
            glAttachShader(_shader_program_object, shader._shader_object);
            return *this;
        }

        template<class... T> inline ShaderProgram& attachShaders(T&&... shader)
        {
            (this->attachShader(shader),...);
            return *this;
        }

        bool link(std::string &info)
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
    
        void release()
        {
            glDeleteProgram(_shader_program_object);
        }
    
        bool operator==(ShaderProgram other)
        {
            return _shader_program_object == other._shader_program_object;
        }
    
        void getLinkMessage(std::string &info)
        {
            char info_buffer[512];
            glGetProgramInfoLog(_shader_program_object, 512, nullptr, info_buffer);
            info = info_buffer;
        }
    
        GLuint rawObject()
        {
            return _shader_program_object;
        }

        enum class SetUniformValueEnum
        {
            SUCCESS,
            INVALID_VALUE,
            INVALID_OPERATION
        };

        static inline SetUniformValueEnum location_judge(GLint location) noexcept
        {
            if(location == GL_INVALID_VALUE) return SetUniformValueEnum::INVALID_VALUE;
            else if(location == GL_INVALID_OPERATION) return SetUniformValueEnum::INVALID_OPERATION;
            return SetUniformValueEnum::SUCCESS;
        }

        /**
         * @brief opengl find uniform variable by name
         * 
         * @tparam T includes std::string std::string_view and const char*
         * @param location 
         * @return SetUniformValueEnum 
         */
        inline SetUniformValueEnum uniformFindLocation(const std::string& name, GLint& location) const
        {
            location = glGetUniformLocation(_shader_program_object, name.c_str());
            return location_judge(location);
        }
        inline SetUniformValueEnum uniformFindLocation(const char* name, GLint& location) const
        {
            location = glGetUniformLocation(_shader_program_object, name);
            return location_judge(location);
        }
        inline SetUniformValueEnum uniformFindLocation(std::string_view name, GLint& location) const
        {
            location = glGetUniformLocation(_shader_program_object, name.data());
            return location_judge(location);
        }

        /**
         * @brief get uniform location without return-value check
         * 
         * @tparam T 
         * @return GLint 
         */
        GLint uniformFindLocationUnsafe(const std::string& name) const
        {
            return glGetUniformLocation(_shader_program_object, name.c_str());
        }
        GLint uniformFindLocationUnsafe(const char* name) const
        {
            return glGetUniformLocation(_shader_program_object, name);
        }
        GLint uniformFindLocationUnsafe(std::string_view name) const
        {
            return glGetUniformLocation(_shader_program_object, name.data());
        }

        template<class T = float, class... VALUES> inline std::enable_if_t<are_same_basic_type<T, VALUES...>::value>
        uniformSetVector(GLint location, VALUES&&... values)
        {
            GLVectorUniformMap<T, sizeof...(VALUES)>::method(location, std::forward<VALUES>(values)...);
        }
        // std::array and eigen::Matrix support
        template<class T, auto LEN> inline void
        uniformSetVector(GLint location, const std::array<T, LEN>& value)
        {
            GLVectorUniformMap<T, LEN>::vmethod(location, 1, value.data());
        }
        template<class T, auto LEN> inline void
        uniformSetVector(GLint location, const Eigen::Vector<T, LEN>& value)
        {
            GLVectorUniformMap<T, LEN>::vmethod(location, 1, value.data());
        }
        // unsafae uniform vector set
        template<class... Values, class STRING_TYPE> inline void
        uniformSetVectorUnsafe(STRING_TYPE&& name, Values&&... values)
        {
            auto location = uniformFindLocationUnsafe(std::forward<STRING_TYPE>(name));
            uniformSetVector(location, std::forward<Values>(values)...);
        }

        template<class T> inline std::enable_if_t<!std::is_class_v<T>>
        uniformSetValue(GLint location, T value)
        {
            using ValueType = std::remove_reference_t<std::remove_const_t<T>>;
            GLVectorUniformMap<ValueType, 1>::method(location, value);
        }
        template<class T, class STRING_TYPE> void inline
        uniformSetValueUnsafe(STRING_TYPE&& name, T value)
        {
            using ValueType = std::remove_reference_t<std::remove_const_t<T>>;
            auto location = uniformFindLocationUnsafe(std::forward<STRING_TYPE>(name));
            GLVectorUniformMap<ValueType, 1>::method(location, value);
        }

        // uniform matrix set
        template<class T, int ROW, int COL> inline std::enable_if_t<!std::is_class_v<T>>
        uniformSetMatrix(GLint location, bool transpose, T* data)
        {
            GLMatrixUniformMap<ROW, COL>::vmethod(location, 1, transpose ? GL_TRUE : GL_FALSE, data);
        }
        template<class T, int ROW, int COL> inline void
        uniformSetMatrix(GLint location, bool transpose, const Eigen::Matrix<T, ROW, COL>& matrix)
        {
            GLMatrixUniformMap<ROW, COL>::vmethod(location, 1, transpose ? GL_TRUE : GL_FALSE, matrix.data());
        }
        template<class T, int DIM, int MODE> void inline
        uniformSetMatrix(GLint location, bool transpose, const Eigen::Transform<T, DIM, MODE>& matrix)
        {
            GLMatrixUniformMap<DIM + 1, DIM + 1>::vmethod(location, 1, transpose ? GL_TRUE : GL_FALSE, matrix.data());
        }
        // unsafe uniform matrix set
        template<class... Values, class STRING_TYPE> void inline
        uniformSetMatrixUnsafe(STRING_TYPE&& name, bool transpose, Values&&... values)
        {
            auto location = uniformFindLocationUnsafe(std::forward<STRING_TYPE>(name));
            uniformSetMatrix(location, transpose, std::forward<Values>(values)...);
        }

    private:
        GLuint _shader_program_object;
    };
}

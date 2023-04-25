#pragma once
#include "config.h"
#include "Shader.hpp"
#include "ShaderUniformType.hpp"
#include <type_traits>
#include <array>

namespace lux::gapi::opengl
{
    class UniformVariableBase
    {
        friend class ShaderProgram;
    public:
        [[nodiscard]] bool isAvailable() const
        {
            return _location != -1;
        }

    protected:
        explicit UniformVariableBase(GLint location)
            : _location(location){}

        GLint _location;
    };

    template<GLSLBuiltinType TSLType, size_t TArraySize>
    class TUniformVariableBase : public UniformVariableBase
    {
        template<typename... Ts> static constexpr bool all_constructible_v 
            = std::conjunction_v<std::is_constructible<typename TUnifromInfo<TSLType>::type, Ts>...>;

        using uniform_info = TUnifromInfo<TSLType>;
        using base_type    = typename uniform_info::type;
        
        using func_info = TUniformFunc<TSLType>;
        static constexpr bool    is_matrix = TUniformFunc<TSLType>::is_matrix;
        static constexpr uint8_t param_num = TUniformFunc<TSLType>::param_num;

    public:
        explicit TUniformVariableBase(GLint location)
            : UniformVariableBase(location){}

        // TODO enable implicit convertion
        /* non-matrix functions */
        // see https ://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html (-fipa-sra)
        template<class... Ts> void setValue(Ts&&... values)
        requires (param_num == sizeof...(Ts) && all_constructible_v<Ts...> && (!is_matrix) && TArraySize == 1)
        {
            func_info::method(_location, std::forward<Ts>(values)...);
        }

        template<class T> void setValue(const T& value)
        requires (param_num > 1, uniform_info::size == sizeof(T) && (!is_matrix) && TArraySize == 1)
        {
            static_assert(std::is_standard_layout_v<T>, "Type should be standard layout");
            func_info::vmethod(_location, 1, (base_type*)&value);
        }

        template<class T> void setValues(const T& value)
        requires (sizeof(T) / uniform_info::size == TArraySize && (!is_matrix) && TArraySize > 1)
        {
            static_assert(std::is_standard_layout_v<T>, "Type should be standard layout");
            func_info::vmethod(_location, sizeof(T) / uniform_info::size, (base_type*)&value);
        }

        void setValues(base_type* data, size_t num)
        requires (!is_matrix)
        {
            func_info::vmethod(_location, num, data);
        }

        /* matrix functions */
        template<class T> void setValues(const T& value, size_t num, bool transpose = false)
        requires (sizeof(T) / uniform_info::size == 0 && is_matrix && TArraySize > 1)
        {
            static_assert(std::is_standard_layout_v<T>, "Type should be standard layout");
            func_info::vmethod(_location, num, transpose, (base_type*)&value);
        }
        
        template<class T> void setValue(const T& value, bool transpose = false)
        requires (uniform_info::size == sizeof(T) && is_matrix && TArraySize == 1)
        {
            static_assert(std::is_standard_layout_v<T>, "Type should be standard layout");
            func_info::vmethod(_location, 1, transpose, (base_type*)&value);
        }

        void setValues(base_type* data, size_t num, bool transpose = false)
        requires is_matrix
        {
            func_info::vmethod(_location, num, transpose, data);
        }
    };

    class ShaderProgram
    {
    public:
        ShaderProgram()
        {
            _shader_program_object = glCreateProgram();
        }

        ShaderProgram(const ShaderProgram&) = delete;
        ShaderProgram& operator=(const ShaderProgram&) = delete;

        ShaderProgram(ShaderProgram&& other) noexcept
        {
            this->_shader_program_object = other._shader_program_object;
            other._shader_program_object = 0;
        }

        ShaderProgram& operator=(ShaderProgram&& other) noexcept
        {
            release();
            this->_shader_program_object = other._shader_program_object;
            other._shader_program_object = 0;
            return *this;
        }

        ~ShaderProgram()
        {
            release();
        }
    
        ShaderProgram& attachShader(GlShaderBase& shader)
        {
            glAttachShader(_shader_program_object, shader.rawObject());
            return *this;
        }

        template<class... T> ShaderProgram& attachShaders(T&&... shader)
        {
            (this->attachShader(shader),...);
            return *this;
        }

        ShaderProgram& detachShader(GlShaderBase& shader)
        {
            glDetachShader(_shader_program_object, shader.rawObject());
            return *this;
        }

        template<class... T> ShaderProgram& detachShaders(T&&... shader)
        {
            (this->detachShader(shader),...);
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
            // If program is zero or a non-zero value that is not the name of a program object
            if(glIsProgram(_shader_program_object) != GL_FALSE)
            {
                glDeleteProgram(_shader_program_object);
                _shader_program_object = 0;
            }
        }
    
        bool operator==(ShaderProgram other) const
        {
            return _shader_program_object == other._shader_program_object;
        }
    
        void getLinkMessage(std::string &info) const
        {
            char info_buffer[512];
            glGetProgramInfoLog(_shader_program_object, 512, nullptr, info_buffer);
            info = info_buffer;
        }
    
        GLuint rawObject()
        {
            return _shader_program_object;
        }

        template<GLSLBuiltinType TSLType, std::size_t TArraySize = 1>
        TUniformVariableBase<TSLType, TArraySize> findUniform(const char* name)
        {
            GLint location = glGetUniformLocation(_shader_program_object, name);
            return TUniformVariableBase<TSLType, TArraySize>(location);
        }

    private:
        GLuint _shader_program_object;
    };
}

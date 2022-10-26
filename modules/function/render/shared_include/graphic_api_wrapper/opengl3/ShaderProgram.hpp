#include "Shader.hpp"
#include <Eigen/Eigen>
#include <array>

namespace lux::engine::function
{
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
    private:
        template<class T, class...>
        struct are_same_basic_type : std::true_type{};

        template<class T, class U, class... TT>
        struct are_same_basic_type<T, U, TT...>
            : std::integral_constant<
        bool, (std::is_same_v<T,U> || std::is_convertible_v<T, U>) 
                && !std::is_class_v<U> && are_same_basic_type<T, TT...>::value>{};

    public:
        ShaderProgram();

        ~ShaderProgram();
    
        ShaderProgram& attachShader(const GlShaderBase& shader);

        template<class... T> ShaderProgram& attachShaders(T&&... shader)
        {
            (this->attachShader(shader),...);
            return *this;
        }

        bool link(std::string& info);

        bool link();

        void use();

        void release();

        bool operator==(ShaderProgram other);

        void getLinkMessage(std::string& info);

        GLuint rawProgramObject();

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
        inline SetUniformValueEnum uniformFindLocation(const std::string& name, GLint& location)
        {
            location = glGetUniformLocation(_shader_program_object, name.c_str());
            return location_judge(location);
        }
        inline SetUniformValueEnum uniformFindLocation(const char* name, GLint& location)
        {
            location = glGetUniformLocation(_shader_program_object, name);
            return location_judge(location);
        }
        inline SetUniformValueEnum uniformFindLocation(std::string_view name, GLint& location)
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
        GLint uniformFindLocationUnsafe(const std::string& name)
        {
            return glGetUniformLocation(_shader_program_object, name.c_str());
        }
        GLint uniformFindLocationUnsafe(const char* name)
        {
            return glGetUniformLocation(_shader_program_object, name);
        }
        GLint uniformFindLocationUnsafe(std::string_view name)
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
        template<class STRING_TYPE, class... Values> inline void
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
        template<class STRING_TYPE, class T> void inline
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
        template<class STRING_TYPE, class... Values> void inline
        uniformSetMatrixUnsafe(STRING_TYPE&& name, bool transpose, Values&&... values)
        {
            auto location = uniformFindLocationUnsafe(std::forward<STRING_TYPE>(name));
            uniformSetMatrix(location, transpose, std::forward<Values>(values)...);
        }

    private:
        GLuint _shader_program_object;
    };
}

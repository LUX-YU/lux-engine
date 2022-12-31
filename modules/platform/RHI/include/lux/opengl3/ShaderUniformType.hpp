#pragma once
#include <glad/glad.h>

namespace lux::gapiwrap::opengl
{
    template<class T, int _LEN> struct GLVectorUniformMap;
    #define GL_VECTOR_MAP_HELPER(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV, TYPE)\
    template<> struct GLVectorUniformMap<TYPE, LEN>{\
        constexpr static auto& method  = GL_UNIFORM_FUNC;\
        constexpr static auto& vmethod = GL_UNIFORM_FUNCV;\
        using value_type = TYPE;\
    };
    #define GL_VECTOR_MAP_HELPER_FLOAT(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV)\
        GL_VECTOR_MAP_HELPER(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV, float)
    #define GL_VECTOR_MAP_HELPER_INT(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV)  \
        GL_VECTOR_MAP_HELPER(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV, int)
    #define GL_VECTOR_MAP_HELPER_UINT(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV) \
        GL_VECTOR_MAP_HELPER(LEN, GL_UNIFORM_FUNC, GL_UNIFORM_FUNCV, unsigned int)

    GL_VECTOR_MAP_HELPER_FLOAT(1,   glUniform1f,  glUniform1fv)  
    GL_VECTOR_MAP_HELPER_FLOAT(2,   glUniform2f,  glUniform2fv)
    GL_VECTOR_MAP_HELPER_FLOAT(3,   glUniform3f,  glUniform3fv)  
    GL_VECTOR_MAP_HELPER_FLOAT(4,   glUniform4f,  glUniform4fv)
    GL_VECTOR_MAP_HELPER_INT  (1,   glUniform1i,  glUniform1iv)  
    GL_VECTOR_MAP_HELPER_INT  (2,   glUniform2i,  glUniform2iv)
    GL_VECTOR_MAP_HELPER_INT  (3,   glUniform3i,  glUniform3iv)  
    GL_VECTOR_MAP_HELPER_INT  (4,   glUniform4i,  glUniform4iv)
    GL_VECTOR_MAP_HELPER_UINT (1,   glUniform1ui, glUniform1uiv) 
    GL_VECTOR_MAP_HELPER_UINT (2,   glUniform2ui, glUniform2uiv)
    GL_VECTOR_MAP_HELPER_UINT (3,   glUniform3ui, glUniform3uiv) 
    GL_VECTOR_MAP_HELPER_UINT (4,   glUniform4ui, glUniform4uiv)

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
    
    GL_MATRIX_MAP_HELPER(2,2, glUniformMatrix2fv)    
    GL_MATRIX_MAP_HELPER(3,3, glUniformMatrix3fv)
    GL_MATRIX_MAP_HELPER(4,4, glUniformMatrix4fv)    
    GL_MATRIX_MAP_HELPER(2,3, glUniformMatrix2x3fv)
    GL_MATRIX_MAP_HELPER(3,2, glUniformMatrix3x2fv)  
    GL_MATRIX_MAP_HELPER(2,4, glUniformMatrix2x4fv)
    GL_MATRIX_MAP_HELPER(4,2, glUniformMatrix4x2fv)  
    GL_MATRIX_MAP_HELPER(3,4, glUniformMatrix3x4fv)
    GL_MATRIX_MAP_HELPER(4,3, glUniformMatrix4x3fv)
}

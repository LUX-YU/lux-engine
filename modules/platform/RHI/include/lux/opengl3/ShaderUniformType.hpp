#pragma once
#include "config.h"
#include <glad/glad.h>
#include <cstdint>
#include <array>

namespace lux::gapi::opengl
{
    // see https://www.khronos.org/opengl/wiki/Data_Type_(GLSL)
    enum class GLSLBuiltinType
    {
        // Basic Types
        BOOL,       // bool
        INT,        // int
        UNINT,      // uint
        FLOAT,      // float
#ifdef __GLPP_SUPPORT_F64
        DOUBLE,     // double 
#endif
        // Vectors
            // bvec(n) boolean vector
        BVECTOR2,   BVECTOR3,   BVECTOR4,
            // ivec(n)  integer vector
        IVECTOR2,   IVECTOR3,   IVECTOR4,
            // uvec(n)  unsigned integer vector
        UVECTOR2,   UVECTOR3,   UVECTOR4,
            // vec(n)   float vector
        FVECTOR2,   FVECTOR3,   FVECTOR4,
            // dvec(n)  double vector
        DVECTOR2,   DVECTOR3,   DVECTOR4,

        // Matrices
        MATRIX2,    MATRIX3,    MATRIX4,
            // matnxm: A matrix with n columns and m rows (examples: mat2x2, mat4x3). 
            // Note that this is backward from convention in mathematics!
        MATRIX_23,  MATRIX_24,
        MATRIX_32,  MATRIX_34,
        MATRIX_42,  MATRIX_43
#ifdef __GLPP_SUPPORT_F64
        // Matrices Double version
        MATRIXD2,    MATRIXD3,  MATRIXD4,
        MATRIXD_23,  MATRIXD_24,
        MATRIXD_32,  MATRIXD_34,
        MATRIXD_42,  MATRIXD_43
#endif
    };

    template<GLSLBuiltinType SL_TYPE> struct TUnifromInfo;
#define TYPE_SIZE_HELPER(TYPE, BASE_TYPE, SIZE)\
    template<> struct TUnifromInfo<TYPE>{\
        static constexpr uint8_t size = SIZE;\
        using  type = BASE_TYPE;\
    };

    TYPE_SIZE_HELPER(GLSLBuiltinType::BOOL,     GLboolean,  sizeof(int32_t)) // boolean is 32 bits
    TYPE_SIZE_HELPER(GLSLBuiltinType::INT,      GLint,      sizeof(int32_t))
    TYPE_SIZE_HELPER(GLSLBuiltinType::UNINT,    GLuint,     sizeof(uint32_t))
    TYPE_SIZE_HELPER(GLSLBuiltinType::FLOAT,    GLfloat,    sizeof(float)) //  IEEE-754
    TYPE_SIZE_HELPER(GLSLBuiltinType::BVECTOR2, GLboolean,  sizeof(int32_t) * 2)
    TYPE_SIZE_HELPER(GLSLBuiltinType::BVECTOR3, GLboolean,  sizeof(int32_t) * 3)
    TYPE_SIZE_HELPER(GLSLBuiltinType::BVECTOR4, GLboolean,  sizeof(int32_t) * 4)
    TYPE_SIZE_HELPER(GLSLBuiltinType::IVECTOR2, GLint,      sizeof(int32_t) * 2)
    TYPE_SIZE_HELPER(GLSLBuiltinType::IVECTOR3, GLint,      sizeof(int32_t) * 3)
    TYPE_SIZE_HELPER(GLSLBuiltinType::IVECTOR4, GLint,      sizeof(int32_t) * 4)
    TYPE_SIZE_HELPER(GLSLBuiltinType::UVECTOR2, GLuint,     sizeof(uint32_t) * 2)
    TYPE_SIZE_HELPER(GLSLBuiltinType::UVECTOR3, GLuint,     sizeof(uint32_t) * 3)
    TYPE_SIZE_HELPER(GLSLBuiltinType::UVECTOR4, GLuint,     sizeof(uint32_t) * 4)
    TYPE_SIZE_HELPER(GLSLBuiltinType::FVECTOR2, GLfloat,    sizeof(float) * 2)
    TYPE_SIZE_HELPER(GLSLBuiltinType::FVECTOR3, GLfloat,    sizeof(float) * 3)
    TYPE_SIZE_HELPER(GLSLBuiltinType::FVECTOR4, GLfloat,    sizeof(float) * 4)

    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX2,  GLfloat,    sizeof(float) * 4)
    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX3,  GLfloat,    sizeof(float) * 9)
    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX4,  GLfloat,    sizeof(float) * 16)
    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX_23,GLfloat,    sizeof(float) * 6)
    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX_24,GLfloat,    sizeof(float) * 8)
    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX_32,GLfloat,    sizeof(float) * 6)
    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX_34,GLfloat,    sizeof(float) * 12)
    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX_42,GLfloat,    sizeof(float) * 8)
    TYPE_SIZE_HELPER(GLSLBuiltinType::MATRIX_43,GLfloat,    sizeof(float) * 12)

// param_num is useless when is_matrix is true
    template<GLSLBuiltinType TSLType> struct TUniformFunc;
#define TYPE_FUNC_HELPER(TYPE, FUNC, VFUNC, IS_MAT, PNUM)\
    template<> struct TUniformFunc<TYPE>{\
        static constexpr auto&   method     = FUNC;\
        static constexpr auto&   vmethod    = VFUNC;\
        static constexpr uint8_t param_num  = PNUM;\
        static constexpr bool    is_matrix  = IS_MAT;\
    };
    TYPE_FUNC_HELPER(GLSLBuiltinType::BOOL,      glUniform1i,           glUniform1iv,   false, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::INT,       glUniform1i,           glUniform1iv,   false, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::UNINT,     glUniform1ui,          glUniform1uiv,  false, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::FLOAT,     glUniform1f,           glUniform1fv,   false, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::BVECTOR2,  glUniform2i,           glUniform2iv,   false, 2)
    TYPE_FUNC_HELPER(GLSLBuiltinType::BVECTOR3,  glUniform3i,           glUniform3iv,   false, 3)
    TYPE_FUNC_HELPER(GLSLBuiltinType::BVECTOR4,  glUniform4i,           glUniform4iv,   false, 4)
    TYPE_FUNC_HELPER(GLSLBuiltinType::IVECTOR2,  glUniform2i,           glUniform2iv,   false, 2)
    TYPE_FUNC_HELPER(GLSLBuiltinType::IVECTOR3,  glUniform3i,           glUniform3iv,   false, 3)
    TYPE_FUNC_HELPER(GLSLBuiltinType::IVECTOR4,  glUniform4i,           glUniform4iv,   false, 4)
    TYPE_FUNC_HELPER(GLSLBuiltinType::UVECTOR2,  glUniform2ui,          glUniform2uiv,  false, 2)
    TYPE_FUNC_HELPER(GLSLBuiltinType::UVECTOR3,  glUniform3ui,          glUniform3uiv,  false, 3)
    TYPE_FUNC_HELPER(GLSLBuiltinType::UVECTOR4,  glUniform4ui,          glUniform4uiv,  false, 4)
    TYPE_FUNC_HELPER(GLSLBuiltinType::FVECTOR2,  glUniform2f,           glUniform2fv,   false, 2)
    TYPE_FUNC_HELPER(GLSLBuiltinType::FVECTOR3,  glUniform3f,           glUniform3fv,   false, 3)
    TYPE_FUNC_HELPER(GLSLBuiltinType::FVECTOR4,  glUniform4f,           glUniform4fv,   false, 4)

    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX2,   glUniformMatrix2fv,    glUniformMatrix2fv,   true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX3,   glUniformMatrix3fv,    glUniformMatrix3fv,   true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX4,   glUniformMatrix4fv,    glUniformMatrix4fv,   true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX_23, glUniformMatrix3x2fv,  glUniformMatrix3x2fv, true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX_24, glUniformMatrix4x2fv,  glUniformMatrix4x2fv, true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX_32, glUniformMatrix2x3fv,  glUniformMatrix2x3fv, true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX_34, glUniformMatrix4x3fv,  glUniformMatrix4x3fv, true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX_42, glUniformMatrix2x4fv,  glUniformMatrix2x4fv, true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIX_43, glUniformMatrix3x4fv,  glUniformMatrix3x4fv, true, 1)

#ifdef __GLPP_SUPPORT_F64
    TYPE_SIZE_HELPER(GLSLBuiltinType::DOUBLE,   GLdouble, sizeof(double))
    TYPE_SIZE_HELPER(GLSLBuiltinType::DVECTOR2, GLdouble, sizeof(double) * 2)
    TYPE_SIZE_HELPER(GLSLBuiltinType::DVECTOR3, GLdouble, sizeof(double) * 3)
    TYPE_SIZE_HELPER(GLSLBuiltinType::DVECTOR4, GLdouble, sizeof(double) * 4)

    TYPE_FUNC_HELPER(GLSLBuiltinType::DOUBLE,    glUniform1d,   false, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::DVECTOR2,  glUniform2d,   false, 2)
    TYPE_FUNC_HELPER(GLSLBuiltinType::DVECTOR3,  glUniform3d,   false, 3)
    TYPE_FUNC_HELPER(GLSLBuiltinType::DVECTOR4,  glUniform4d,   false, 4)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD2,  glUniformMatrix2dv,    true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD3,  glUniformMatrix3dv,    true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD4,  glUniformMatrix4dv,    true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD_23,glUniformMatrix3x2dv,  true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD_24,glUniformMatrix4x2dv,  true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD_32,glUniformMatrix2x3dv,  true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD_34,glUniformMatrix4x3dv,  true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD_42,glUniformMatrix2x4dv,  true, 1)
    TYPE_FUNC_HELPER(GLSLBuiltinType::MATRIXD_43,glUniformMatrix3x4dv,  true, 1)
#endif
}

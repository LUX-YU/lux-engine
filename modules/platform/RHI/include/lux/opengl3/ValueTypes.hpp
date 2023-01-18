#pragma once
#include <glad/glad.h>
#include <cstdint>

namespace lux::gapi::opengl
{
    // mark(tr) means this format are required for both textures and renderbuffers
    // these format are written in https://www.khronos.org/opengl/wiki/Image_Format
    enum class ImageFormat
    {
        /* Unsized Format */
        
        DEPTH_STENCIL   = GL_DEPTH_STENCIL,     // Depth stencil formats
        DEPTH_COMPONENT = GL_DEPTH_COMPONENT,
        RED             = GL_RED,
        GREEN           = GL_GREEN,
        BLUE            = GL_BLUE,
        ALPHA           = GL_ALPHA,             // GL_UNSIGNED_BYTE
        RGB             = GL_RGB,               // GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT_5_6_5
        RGBA            = GL_RGBA,              // GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_SHORT_5_5_5_1
        LUMINANCE       = GL_LUMINANCE,         // GL_UNSIGNED_BYTE
        LUMINANCE_ALPHA = GL_LUMINANCE_ALPHA,   // GL_UNSIGNED_BYTE

        // Color formats
        // OpenGL has a particular syntax for writing its color format enumerants. It looks like this:
        // GL_[components​][size​][type​]
        
        RED_INTEGER     = GL_RED_INTEGER,
        RG              = GL_RG,
        RG_INTEGER      = GL_RG_INTEGER,
        
        
        R8              = GL_R8,
        R8_SNORM        = GL_R8_SNORM,
        R16             = GL_R16,
        R16_SNORM       = GL_R16_SNORM,
        RG8             = GL_RG8,
        RG8_SNORM       = GL_RG8_SNORM,
        RG16            = GL_RG16,
        RG16_SNORM      = GL_RG16_SNORM,
        RGB4            = GL_RGB4,
        RGBA8           = GL_RGBA8,
        RGB5            = GL_RGB5,
        RGB8            = GL_RGB8,
        RGB8_SNORM      = GL_RGB8_SNORM,
        RGB10           = GL_RGB10,
        RGB12           = GL_RGB12,
        RGB16_SNORM     = GL_RGB16_SNORM,
        RGBA2           = GL_RGBA2,
        RGBA4           = GL_RGBA4,/*tr opengl 4.2*/
        RGBA8_SNORM     = GL_RGBA8_SNORM,
        RGBA12          = GL_RGBA12,
        RGBA16          = GL_RGBA16,
        SRGB8_ALPHA8    = GL_SRGB8_ALPHA8,/*tr*/
        RG16F           = GL_RG16F,
        RGB16F          = GL_RGB16F,
        RGBA16F         = GL_RGBA16F,
        R32F            = GL_R32F,
        RG32F           = GL_RG32F,
        RGB32F          = GL_RGB32F,
        RGBA32F         = GL_RGBA32F,
        R8I             = GL_R8I,
        R8UI            = GL_R8UI,
        R16I            = GL_R16I,
        R16UI           = GL_R16UI,
        R32I            = GL_R32I,
        R32UI           = GL_R32UI,
        RG8I            = GL_RG8I,
        RG8UI           = GL_RG8UI,
        RG16I           = GL_RG16I,
        RG16UI          = GL_RG16UI,
        RG32I           = GL_RG32I,
        RG32UI          = GL_RG32UI,
        RGB8I           = GL_RGB8I,
        RGB8UI          = GL_RGB8UI,
        RGB16I          = GL_RGB16I,
        RGB16UI         = GL_RGB16UI,
        RGB32I          = GL_RGB32I,
        RGB32UI         = GL_RGB32UI,
        RGBA8I          = GL_RGBA8I,
        RGBA8UI         = GL_RGBA8UI,
        RGBA16I         = GL_RGBA16I,
        RGBA16UI        = GL_RGBA16UI,
        RGBA32I         = GL_RGBA32I,
        RGBA32UI        = GL_RGBA32UI,
        R16F            = GL_R16F,
        RGB_INTEGER     = GL_RGB_INTEGER,
        BGR_INTEGER     = GL_BGR_INTEGER,
        RGBA_INTEGER    = GL_RGBA_INTEGER,
        BGRA_INTEGER    = GL_BGRA_INTEGER,
            // Special(not obey name rule)
        R3_G3_B2        = GL_R3_G3_B2,
        RGB5_A1         = GL_RGB5_A1,/*tr opengl 4.2*/
        RGB10_A2        = GL_RGB10_A2,/*tr*/ 
        RGB10_A2UI      = GL_RGB10_A2UI,/*tr*/
        R11F_G11F_B10F  = GL_R11F_G11F_B10F,/*tr*/ 
        RGB9_E5         = GL_RGB9_E5,
        RGB565          = GL_RGB565,/*tr opengl4.1 or ARB_ES2_compatibility*/
            // sRGB colorspace
        SRGB8           = GL_SRGB8,            

        // Compressed formats
        COMPRESSED_RED                      = GL_COMPRESSED_RED,
        COMPRESSED_RG                       = GL_COMPRESSED_RG,
        COMPRESSED_RGB                      = GL_COMPRESSED_RGB,
        COMPRESSED_RGBA                     = GL_COMPRESSED_RGBA,
            // sRGB
        COMPRESSED_SRGB                     = GL_COMPRESSED_SRGB,
        COMPRESSED_SRGB_ALPHA               = GL_COMPRESSED_SRGB_ALPHA,
            // Red/Green compressed formats:
        COMPRESSED_RED_RGTC1                = GL_COMPRESSED_RED_RGTC1,
        COMPRESSED_SIGNED_RED_RGTC1         = GL_COMPRESSED_SIGNED_RED_RGTC1,
        COMPRESSED_RG_RGTC2                 = GL_COMPRESSED_RG_RGTC2,
        COMPRESSED_SIGNED_RG_RGTC2          = GL_COMPRESSED_SIGNED_RG_RGTC2,
            // BPTC compressed formats 
        COMPRESSED_RGBA_BPTC_UNORM          = GL_COMPRESSED_RGBA_BPTC_UNORM,
        COMPRESSED_SRGB_ALPHA_BPTC_UNORM    = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM,
        COMPRESSED_RGB_BPTC_SIGNED_FLOAT    = GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT,
        COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT  = GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT,

        // Depth formats
        DEPTH_COMPONENT16       = GL_DEPTH_COMPONENT16,/*tr*/   
        DEPTH_COMPONENT24       = GL_DEPTH_COMPONENT24,/*tr*/
        DEPTH_COMPONENT32       = GL_DEPTH_COMPONENT32,        
        DEPTH_COMPONENT32F      = GL_DEPTH_COMPONENT32F,/*tr*/

        // There are only 2 depth/stencil formats,each providing 8 stencil bits
        DEPTH24_STENCIL8        = GL_DEPTH24_STENCIL8,/*tr*/
        DEPTH32F_STENCIL8       = GL_DEPTH32F_STENCIL8,/*tr*/

        // Stencil only
        STENCIL_INDEX           = GL_STENCIL_INDEX,
        STENCIL_INDEX1          = GL_STENCIL_INDEX1,
        STENCIL_INDEX4          = GL_STENCIL_INDEX4,
        STENCIL_INDEX8          = GL_STENCIL_INDEX8,/*tr*/
        STENCIL_INDEX16         = GL_STENCIL_INDEX16,
    };

    enum class CompareFunc
    {
        LEQUAL                      =   GL_LEQUAL,
        GEQUAL                      =   GL_GEQUAL,
        LESS                        =   GL_LESS,
        GREATER                     =   GL_GREATER,
        EQUAL                       =   GL_EQUAL,
        NOTEQUAL                    =   GL_NOTEQUAL,
        ALWAYS                      =   GL_ALWAYS,
        NEVER                       =   GL_NEVER
    };

    enum class DataType
    {
        // BASIC TYPE
        BYTE                                = GL_BYTE,
        UNSIGNED_BYTE                       = GL_UNSIGNED_BYTE,
        SHORT                               = GL_SHORT,
        UNSIGNED_SHORT                      = GL_UNSIGNED_SHORT,
        INT                                 = GL_INT,
        UNSIGNED_INT                        = GL_UNSIGNED_INT,
        FLOAT                               = GL_FLOAT,
        BYTES2                              = GL_2_BYTES,
        BYTES3                              = GL_3_BYTES,
        BYTES4                              = GL_4_BYTES,
        DOUBLE                              = GL_DOUBLE,
        HALF_FLOAT                          = GL_HALF_FLOAT,
        FIXED                               = GL_FIXED,
        
        // Splite Data
        UNSIGNED_BYTE_3_3_2                 = GL_UNSIGNED_BYTE_3_3_2,
        UNSIGNED_SHORT_4_4_4_4              = GL_UNSIGNED_SHORT_4_4_4_4,
        UNSIGNED_SHORT_5_5_5_1              = GL_UNSIGNED_SHORT_5_5_5_1,
        UNSIGNED_INT_24_8                   = GL_UNSIGNED_INT_24_8,
        UNSIGNED_INT_8_8_8_8                = GL_UNSIGNED_INT_8_8_8_8,
        UNSIGNED_INT_10_10_10_2             = GL_UNSIGNED_INT_10_10_10_2,
        UNSIGNED_SHORT_5_6_5                = GL_UNSIGNED_SHORT_5_6_5,

        // REV
        INT_2_10_10_10_REV                  = GL_INT_2_10_10_10_REV,
        UNSIGNED_BYTE_2_3_3_REV             = GL_UNSIGNED_BYTE_2_3_3_REV,
        UNSIGNED_SHORT_5_6_5_REV            = GL_UNSIGNED_SHORT_5_6_5_REV,
        UNSIGNED_SHORT_4_4_4_4_REV          = GL_UNSIGNED_SHORT_4_4_4_4_REV,
        UNSIGNED_SHORT_1_5_5_5_REV          = GL_UNSIGNED_SHORT_1_5_5_5_REV,
        UNSIGNED_INT_5_9_9_9_REV            = GL_UNSIGNED_INT_5_9_9_9_REV,
        UNSIGNED_INT_8_8_8_8_REV            = GL_UNSIGNED_INT_8_8_8_8_REV,
        UNSIGNED_INT_2_10_10_10_REV         = GL_UNSIGNED_INT_2_10_10_10_REV,
        UNSIGNED_INT_10F_11F_11F_REV        = GL_UNSIGNED_INT_10F_11F_11F_REV
    };

    // type size
    // https://www.khronos.org/opengl/wiki/OpenGL_Type
    template<DataType dtype> struct TDataTypeMap;
#define DATA_TYPE_SIZE(TYPE, GLTYPE)\
    template<> struct TDataTypeMap<TYPE>{using type = GLTYPE; static constexpr int size = sizeof(GLTYPE);};
    DATA_TYPE_SIZE(DataType::BYTE,                          GLbyte)
    DATA_TYPE_SIZE(DataType::UNSIGNED_BYTE,                 GLubyte)
    DATA_TYPE_SIZE(DataType::SHORT,                         GLshort)
    DATA_TYPE_SIZE(DataType::UNSIGNED_SHORT,                GLushort)
    DATA_TYPE_SIZE(DataType::INT,                           GLint)
    DATA_TYPE_SIZE(DataType::UNSIGNED_INT,                  GLuint)
    DATA_TYPE_SIZE(DataType::FLOAT,                         GLfloat)
    DATA_TYPE_SIZE(DataType::BYTES2,                        GLbyte[2]) // can't find builtin type
    DATA_TYPE_SIZE(DataType::BYTES3,                        GLbyte[3]) // can't find builtin type
    DATA_TYPE_SIZE(DataType::BYTES4,                        GLbyte[4]) // can't find builtin type
    DATA_TYPE_SIZE(DataType::DOUBLE,                        GLdouble)
    DATA_TYPE_SIZE(DataType::HALF_FLOAT,                    GLhalf)
    DATA_TYPE_SIZE(DataType::FIXED,                         GLfixed)
    DATA_TYPE_SIZE(DataType::UNSIGNED_BYTE_3_3_2,           GLubyte)
    DATA_TYPE_SIZE(DataType::UNSIGNED_SHORT_4_4_4_4,        GLushort)
    DATA_TYPE_SIZE(DataType::UNSIGNED_SHORT_5_5_5_1,        GLushort)
    DATA_TYPE_SIZE(DataType::UNSIGNED_INT_8_8_8_8,          GLuint)
    DATA_TYPE_SIZE(DataType::UNSIGNED_INT_10_10_10_2,       GLuint)
    DATA_TYPE_SIZE(DataType::UNSIGNED_SHORT_5_6_5,          GLushort)
    DATA_TYPE_SIZE(DataType::INT_2_10_10_10_REV,            GLint)
    DATA_TYPE_SIZE(DataType::UNSIGNED_BYTE_2_3_3_REV,       GLubyte)
    DATA_TYPE_SIZE(DataType::UNSIGNED_SHORT_5_6_5_REV,      GLushort)
    DATA_TYPE_SIZE(DataType::UNSIGNED_SHORT_4_4_4_4_REV,    GLushort)
    DATA_TYPE_SIZE(DataType::UNSIGNED_SHORT_1_5_5_5_REV,    GLushort)
    DATA_TYPE_SIZE(DataType::UNSIGNED_INT_5_9_9_9_REV,      GLuint)
    DATA_TYPE_SIZE(DataType::UNSIGNED_INT_8_8_8_8_REV,      GLuint)
    DATA_TYPE_SIZE(DataType::UNSIGNED_INT_2_10_10_10_REV,   GLuint)
    DATA_TYPE_SIZE(DataType::UNSIGNED_INT_10F_11F_11F_REV,  GLuint)


} // namespace lux::gapi::opengl

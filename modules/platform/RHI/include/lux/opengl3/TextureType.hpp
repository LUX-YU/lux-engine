#pragma once
#include <glad/glad.h>

namespace lux::gapiwrap::opengl{

    enum class TextureType
    {
        TEXTURE_1D                  = GL_TEXTURE_1D,
        TEXTURE_2D                  = GL_TEXTURE_2D,
        TEXTURE_3D                  = GL_TEXTURE_3D,
        TEXTURE_1D_ARRAY            = GL_TEXTURE_1D_ARRAY,
        TEXTURE_2D_ARRAY            = GL_TEXTURE_2D_ARRAY,
        TEXTURE_RECTANGLE           = GL_TEXTURE_RECTANGLE,
        TEXTURE_CUBE_MAP            = GL_TEXTURE_CUBE_MAP,
        TEXTURE_CUBE_MAP_ARRAY      = GL_TEXTURE_CUBE_MAP_ARRAY,
        TEXTURE_BUFFER              = GL_TEXTURE_BUFFER,
        TEXTURE_2D_MULTISAMPLE      = GL_TEXTURE_2D_MULTISAMPLE,
        TEXTURE_2D_MULTISAMPLE_ARRAY= GL_TEXTURE_2D_MULTISAMPLE_ARRAY
    };

    // PName introduction see 
    // https://registry.khronos.org/OpenGL-Refpages/gl4/html/glTexParameter.xhtml
    enum class TexturePName : GLenum
    {
        // TEXTURE_PNAME_VALUE_DEPTH_STENCIL_TEXTURE_MODE
        DEPTH_STENCIL_TEXTURE_MODE  = GL_DEPTH_STENCIL_TEXTURE_MODE,
        // int, This is an integer value. The initial value is 0.
        TEXTURE_BASE_LEVEL          = GL_TEXTURE_BASE_LEVEL,
        // float[4]
        TEXTURE_BORDER_COLOR        = GL_TEXTURE_BORDER_COLOR,
        // PNAME_VALUE_COMPARE_FUNC
        TEXTURE_COMPARE_FUNC        = GL_TEXTURE_COMPARE_FUNC,
        // PNAME_VALUE_COMPARE_MODE
        TEXTURE_COMPARE_MODE        = GL_TEXTURE_COMPARE_MODE,
        // int
        TEXTURE_LOD_BIAS            = GL_TEXTURE_LOD_BIAS,
        // PNAME_VALUE_MIN_FILTER
        TEXTURE_MIN_FILTER          = GL_TEXTURE_MIN_FILTER,
        // TEXTURE_PNAME_VALUE_MAG_FILTER
        TEXTURE_MAG_FILTER          = GL_TEXTURE_MAG_FILTER,
        // float
        // -1000
        TEXTURE_MIN_LOD             = GL_TEXTURE_MIN_LOD,
        // float
        // 1000
        TEXTURE_MAX_LOD             = GL_TEXTURE_MAX_LOD,
        // int
        // 1000
        TEXTURE_MAX_LEVEL           = GL_TEXTURE_MAX_LEVEL,
        // TEXTURE_PNAME_VALUE_SWIZZLE
        // The initial value is GL_RED
        TEXTURE_SWIZZLE_R           = GL_TEXTURE_SWIZZLE_R,
        // TEXTURE_PNAME_VALUE_SWIZZLE
        // The initial value is GL_GREEN
        TEXTURE_SWIZZLE_G           = GL_TEXTURE_SWIZZLE_G,
        // TEXTURE_PNAME_VALUE_SWIZZLE
        // The initial value is GL_BLUE
        TEXTURE_SWIZZLE_B           = GL_TEXTURE_SWIZZLE_B,
        // TEXTURE_PNAME_VALUE_SWIZZLE
        // The initial value is GL_ALPHA
        TEXTURE_SWIZZLE_A           = GL_TEXTURE_SWIZZLE_A,
        // TEXTURE_PNAME_VALUE_SWIZZLE
        TEXTURE_SWIZZLE_RGBA        = GL_TEXTURE_SWIZZLE_RGBA,
        // TEXTURE_PNAME_VALUE_WRAP
        // GL_REPEAT
        TEXTURE_WRAP_S              = GL_TEXTURE_WRAP_S,
        // TEXTURE_PNAME_VALUE_WRAP
        // GL_REPEAT
        TEXTURE_WRAP_T              = GL_TEXTURE_WRAP_T,
        // TEXTURE_PNAME_VALUE_WRAP
        // GL_REPEAT
        TEXTURE_WRAP_R              = GL_TEXTURE_WRAP_R
    };

    enum class TEXTURE_PNAME_VALUE_DEPTH_STENCIL_TEXTURE_MODE
    {
        // then reads from depth-stencil format 
        // textures will return the depth component of the 
        // texel in Rt and the stencil component will be discarded
        DEPTH_COMPONENT = GL_DEPTH_COMPONENT,
        STENCIL_INDEX   = GL_STENCIL_INDEX
    };

    enum class TEXTURE_PNAME_VALUE_COMPARE_FUNC
    {
        LEQUAL  =   GL_LEQUAL,
        GEQUAL  =   GL_GEQUAL,
        LESS    =   GL_LESS,
        GREATER =   GL_GREATER,
        EQUAL   =   GL_EQUAL,
        NOTEQUAL=   GL_NOTEQUAL,
        ALWAYS  =   GL_ALWAYS,
        NEVER   =   GL_NEVER
    };

    enum class TEXTURE_PNAME_VALUE_COMPARE_MODE
    {
        COMPARE_REF_TO_TEXTURE = GL_COMPARE_REF_TO_TEXTURE,
        NONE                   = GL_NONE
    };

    enum class TEXTURE_PNAME_VALUE_MIN_FILTER : GLuint
    {
        // Returns the value of the texture element that is nearest (in Manhattan distance) to the specified texture coordinates.
        NEAREST                 = GL_NEAREST,
        // Returns the weighted average of the four texture elements that are closest to the specified texture coordinates. 
        // These can include items wrapped or repeated from other parts of a texture, 
        // depending on the values of GL_TEXTURE_WRAP_S and GL_TEXTURE_WRAP_T, and on the exact mapping.
        LINEAR                  = GL_LINEAR,
        // Chooses the mipmap that most closely matches the size of the pixel being textured and uses 
        // the GL_NEAREST criterion (the texture element closest to the specified texture coordinates) to produce a texture value.
        NEAREST_MIPMAP_NEAREST  = GL_NEAREST_MIPMAP_NEAREST,
        // Chooses the mipmap that most closely matches the size of the pixel being textured and uses the GL_LINEAR criterion 
        // (a weighted average of the four texture elements that are closest to the specified texture coordinates) to produce a texture value.
        LINEAR_MIPMAP_NEAREST   = GL_LINEAR_MIPMAP_NEAREST,
        // Chooses the two mipmaps that most closely match the size of the pixel being textured and uses the GL_NEAREST criterion 
        // (the texture element closest to the specified texture coordinates ) to produce a texture value from each mipmap. 
        // The final texture value is a weighted average of those two values.
        NEAREST_MIPMAP_LINEAR   = GL_NEAREST_MIPMAP_LINEAR,
        // Chooses the two mipmaps that most closely match the size of the pixel being textured and uses the GL_LINEAR criterion 
        // (a weighted average of the texture elements that are closest to the specified texture coordinates) to produce a texture 
        // value from each mipmap. The final texture value is a weighted average of those two values.
        LINEAR_MIPMAP_LINEA     = GL_LINEAR_MIPMAP_LINEAR
    };

    // GL_NEAREST is generally faster than GL_LINEAR, but it can produce textured images with sharper edges because the transition 
    // between texture elements is not as smooth. The initial value of GL_TEXTURE_MAG_FILTER is GL_LINEAR.
    enum class TEXTURE_PNAME_VALUE_MAG_FILTER : GLuint
    {
        // Returns the value of the texture element that is nearest (in Manhattan distance) to the specified texture coordinates.
        NEAREST = GL_NEAREST,
        // Returns the weighted average of the texture elements that are closest to the specified texture coordinates. 
        // These can include items wrapped or repeated from other parts of a texture, 
        // depending on the values of GL_TEXTURE_WRAP_S and GL_TEXTURE_WRAP_T, and on the exact mapping.
        LINEAR  = GL_LINEAR 
    };

    enum class TEXTURE_PNAME_VALUE_SWIZZLE : GLuint
    {
        RED     = GL_RED,
        GREEN   = GL_GREEN,
        BLUE    = GL_BLUE,
        ALPHA   = GL_ALPHA,
        ZERO    = GL_ZERO,
        ONE     = GL_ONE
    };

    enum class TEXTURE_PNAME_VALUE_WRAP : GLuint
    {
        CLAMP_TO_EDGE           = GL_CLAMP_TO_EDGE,
        CLAMP_TO_BORDER         = GL_CLAMP_TO_BORDER,
        MIRRORED_REPEAT         = GL_MIRRORED_REPEAT,
        REPEAT                  = GL_REPEAT,
        MIRROR_CLAMP_TO_EDGE    = GL_MIRROR_CLAMP_TO_EDGE
    };

    template<class T> struct TextureSetParameterTypeMap;
#define TEXTURE_SET_PARAMETER_TYPE_MAP(GL_TEX_FUNC, GL_TEXTURE_FUNC, TYPE, TRUE_TYPE)\
    template<> struct TextureSetParameterTypeMap<TYPE>{\
        constexpr static auto& tex_method = GL_TEX_FUNC;\
        constexpr static auto& texture_method = GL_TEXTURE_FUNC;\
        using value_type = TYPE;\
        using true_type = TRUE_TYPE;\
    };

    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameteri,  glTextureParameteri,  TEXTURE_PNAME_VALUE_DEPTH_STENCIL_TEXTURE_MODE, GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameteri,  glTextureParameteri,  TEXTURE_PNAME_VALUE_COMPARE_FUNC, GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameteri,  glTextureParameteri,  TEXTURE_PNAME_VALUE_COMPARE_MODE, GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameteri,  glTextureParameteri,  TEXTURE_PNAME_VALUE_MIN_FILTER, GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameteri,  glTextureParameteri,  TEXTURE_PNAME_VALUE_MAG_FILTER, GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameteri,  glTextureParameteri,  TEXTURE_PNAME_VALUE_SWIZZLE, GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameteri,  glTextureParameteri,  TEXTURE_PNAME_VALUE_WRAP, GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameteri,  glTextureParameteri,  GLint, GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameterf,  glTextureParameterf,  GLfloat, GLfloat)
    TEXTURE_SET_PARAMETER_TYPE_MAP(glTexParameterfv, glTextureParameterfv, GLfloat[4], GLfloat[4])

    template<TexturePName> struct TexPNameValueType;
#define TEXTURE_PNAME_VALUE_TYPE_MAP(pname, value_type)\
    template<> struct TexPNameValueType<pname>{\
        using type = value_type;\
    };

    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::DEPTH_STENCIL_TEXTURE_MODE, TEXTURE_PNAME_VALUE_DEPTH_STENCIL_TEXTURE_MODE)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_BASE_LEVEL        , GLint)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_BORDER_COLOR      , GLfloat[4])
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_COMPARE_FUNC      , TEXTURE_PNAME_VALUE_COMPARE_FUNC)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_COMPARE_MODE      , TEXTURE_PNAME_VALUE_COMPARE_MODE)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_LOD_BIAS          , GLfloat)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_MIN_FILTER        , TEXTURE_PNAME_VALUE_MIN_FILTER)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_MAG_FILTER        , TEXTURE_PNAME_VALUE_MAG_FILTER)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_MIN_LOD           , GLfloat)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_MAX_LOD           , GLfloat)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_MAX_LEVEL         , GLint)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_SWIZZLE_R         , TEXTURE_PNAME_VALUE_SWIZZLE)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_SWIZZLE_G         , TEXTURE_PNAME_VALUE_SWIZZLE)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_SWIZZLE_B         , TEXTURE_PNAME_VALUE_SWIZZLE)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_SWIZZLE_A         , TEXTURE_PNAME_VALUE_SWIZZLE)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_SWIZZLE_RGBA      , TEXTURE_PNAME_VALUE_SWIZZLE)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_WRAP_S            , TEXTURE_PNAME_VALUE_WRAP)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_WRAP_T            , TEXTURE_PNAME_VALUE_WRAP)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::TEXTURE_WRAP_R            , TEXTURE_PNAME_VALUE_WRAP)

    // https://registry.khronos.org/OpenGL-Refpages/gl4/html/glTexParameter.xhtml
    template<TextureType _ttype> concept parameter_condition = requires {
        _ttype == TextureType::TEXTURE_1D                      || 
        _ttype == TextureType::TEXTURE_1D_ARRAY                ||
        _ttype == TextureType::TEXTURE_2D                      || 
        _ttype == TextureType::TEXTURE_2D_ARRAY                ||
        _ttype == TextureType::TEXTURE_2D_MULTISAMPLE          || 
        _ttype == TextureType::TEXTURE_2D_MULTISAMPLE_ARRAY    ||
        _ttype == TextureType::TEXTURE_3D                      || 
        _ttype == TextureType::TEXTURE_CUBE_MAP                ||
        _ttype == TextureType::TEXTURE_CUBE_MAP_ARRAY          || 
        _ttype == TextureType::TEXTURE_RECTANGLE;
    };

    enum class TextureBaseInternalFormat
    {
        DEPTH_COMPONENT = GL_DEPTH_COMPONENT,
        DEPTH_STENCIL = GL_DEPTH_STENCIL,
        RED = GL_RED,
        RG = GL_RG,
        RGB = GL_RGB,
        RGBA = GL_RGBA
    };

    enum class TextureSizedInternalFormat
    {
        R8 = GL_R8,                     R8_SNORM = GL_R8_SNORM,
        R16 = GL_R16,                   R16_SNORM = GL_R16_SNORM,
        RG8 = GL_RG8,                   RG8_SNORM = GL_RG8_SNORM,
        RG16 = GL_RG16,                 RG16_SNORM = GL_RG16_SNORM,
        R3_G3_B2 = GL_R3_G3_B2,         RGB4 = GL_RGB4,
        RGB5 = GL_RGB5,                 RGB8 = GL_RGB8,
        RGB8_SNORM = GL_RGB8_SNORM,     RGB10 = GL_RGB10,
        RGB12 = GL_RGB12,               RGB16_SNORM = GL_RGB16_SNORM,
        RGBA2 = GL_RGBA2,               RGBA4 = GL_RGBA4,
        RGB5_A1 = GL_RGB5_A1,           RGBA8 = GL_RGBA8,
        RGBA8_SNORM = GL_RGBA8_SNORM,   RGB10_A2 = GL_RGB10_A2,
        RGB10_A2UI = GL_RGB10_A2UI,     RGBA12 = GL_RGBA12,
        RGBA16 = GL_RGBA16,             SRGB8 = GL_SRGB8,
        SRGB8_ALPHA8 = GL_SRGB8_ALPHA8, R16F = GL_R16F,
        RG16F = GL_RG16F,               RGB16F = GL_RGB16F,
        RGBA16F = GL_RGBA16F,           R32F = GL_R32F,
        RG32F = GL_RG32F,               RGB32F = GL_RGB32F,
        RGBA32F = GL_RGBA32F,           R11F_G11F_B10F = GL_R11F_G11F_B10F,
        RGB9_E5 = GL_RGB9_E5,           R8I = GL_R8I,
        R8UI = GL_R8UI,                 R16I = GL_R16I,
        R16UI = GL_R16UI,               R32I = GL_R32I,
        R32UI = GL_R32UI,               RG8I = GL_RG8I,
        RG8UI = GL_RG8UI,               RG16I = GL_RG16I,
        RG16UI = GL_RG16UI,             RG32I = GL_RG32I,
        RG32UI = GL_RG32UI,             RGB8I = GL_RGB8I,
        RGB8UI = GL_RGB8UI,             RGB16I = GL_RGB16I,
        RGB16UI = GL_RGB16UI,           RGB32I = GL_RGB32I,
        RGB32UI = GL_RGB32UI,           RGBA8I = GL_RGBA8I,
        RGBA8UI = GL_RGBA8UI,           RGBA16I = GL_RGBA16I,
        RGBA16UI = GL_RGBA16UI,         RGBA32I = GL_RGBA32I,
        RGBA32UI = GL_RGBA32UI
    };

    enum class TextureCompressedInternalFormat
    {
        COMPRESSED_RED                      = GL_COMPRESSED_RED,
        COMPRESSED_RG                       = GL_COMPRESSED_RG,
        COMPRESSED_RGB                      = GL_COMPRESSED_RGB,
        COMPRESSED_RGBA                     = GL_COMPRESSED_RGBA,
        COMPRESSED_SRGB                     = GL_COMPRESSED_SRGB,
        COMPRESSED_SRGB_ALPHA               = GL_COMPRESSED_SRGB_ALPHA,
        COMPRESSED_RED_RGTC1                = GL_COMPRESSED_RED_RGTC1,
        COMPRESSED_SIGNED_RED_RGTC1         = GL_COMPRESSED_SIGNED_RED_RGTC1,
        COMPRESSED_RG_RGTC2                 = GL_COMPRESSED_RG_RGTC2,
        COMPRESSED_SIGNED_RG_RGTC2          = GL_COMPRESSED_SIGNED_RG_RGTC2,
        COMPRESSED_RGBA_BPTC_UNORM          = GL_COMPRESSED_RGBA_BPTC_UNORM,
        COMPRESSED_SRGB_ALPHA_BPTC_UNORM    = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM,
        COMPRESSED_RGB_BPTC_SIGNED_FLOAT    = GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT,
        COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT  = GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT,
    };
}
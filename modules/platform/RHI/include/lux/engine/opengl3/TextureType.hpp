#pragma once
#include <glad/glad.h>
#include <type_traits>
#include "ValueTypes.hpp"

namespace lux::gapi::opengl{
    enum class TextureType : GLenum
    {
        ONE_DIM                     = GL_TEXTURE_1D,
        ONE_DIM_ARRAY               = GL_TEXTURE_1D_ARRAY,
        THREE_DIM                   = GL_TEXTURE_3D,
        CUBE_MAP                    = GL_TEXTURE_CUBE_MAP,
        CUBE_MAP_ARRAY              = GL_TEXTURE_CUBE_MAP_ARRAY,
        TWO_DIM                     = GL_TEXTURE_2D,
        TWO_DIM_ARRAY               = GL_TEXTURE_2D_ARRAY,
        TWO_DIM_MULTISAMPLE         = GL_TEXTURE_2D_MULTISAMPLE,
        TWO_DIM_MULTISAMPLE_ARRAY   = GL_TEXTURE_2D_MULTISAMPLE_ARRAY,
        RECTANGLE                   = GL_TEXTURE_RECTANGLE,
        BUFFER                      = GL_TEXTURE_BUFFER, // no proxy

        // proxy
        PROXY_ONE_DIM               = GL_PROXY_TEXTURE_1D,
        PROXY_ONE_DIM_ARRAY         = GL_PROXY_TEXTURE_1D_ARRAY,
        PROXY_TWO_DIM               = GL_PROXY_TEXTURE_2D,
        PROXY_TWO_DIM_ARRAY         = GL_PROXY_TEXTURE_2D_ARRAY,
        PROXY_TWO_DIM_MULTISAMPLE   = GL_PROXY_TEXTURE_2D_MULTISAMPLE,
        PROXY_TWO_DIM_MULTISAMPLE_ARRAY = GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY,
        PROXY_THREE_DIM             = GL_PROXY_TEXTURE_3D,
        PROXY_CUBE_MAP              = GL_PROXY_TEXTURE_CUBE_MAP,
        PROXY_CUBE_MAP_ARRAY        = GL_PROXY_TEXTURE_CUBE_MAP_ARRAY,
        PROXY_RECTANGLE             = GL_PROXY_TEXTURE_RECTANGLE
    };

    template<TextureType ttype> concept texnotproxy =
        ttype != TextureType::PROXY_ONE_DIM         &&
        ttype != TextureType::PROXY_ONE_DIM_ARRAY   &&
        ttype != TextureType::PROXY_TWO_DIM         &&
        ttype != TextureType::PROXY_TWO_DIM_ARRAY   &&
        ttype != TextureType::PROXY_TWO_DIM_MULTISAMPLE &&
        ttype != TextureType::PROXY_TWO_DIM_MULTISAMPLE_ARRAY &&
        ttype != TextureType::PROXY_THREE_DIM       &&
        ttype != TextureType::PROXY_CUBE_MAP        &&
        ttype != TextureType::PROXY_CUBE_MAP_ARRAY  &&
        ttype != TextureType::PROXY_RECTANGLE;

    template<TextureType ttype> concept texmipmap =
        ttype == TextureType::ONE_DIM               ||
        ttype == TextureType::TWO_DIM               ||
        ttype == TextureType::THREE_DIM             ||
        ttype == TextureType::ONE_DIM_ARRAY         ||
        ttype == TextureType::TWO_DIM_ARRAY         ||
        ttype == TextureType::CUBE_MAP              ||
        ttype == TextureType::CUBE_MAP_ARRAY;

    template<TextureType ttype> concept bindable = texnotproxy<ttype>;

    template<TextureType Type> struct TextureProxyValueMap;
#define TEXTURE_SET_PROXY_VALUE(TEXUTR_TYPE, VALUE)\
    template<> struct TextureProxyValueMap<TEXUTR_TYPE>{static constexpr TextureType value = VALUE;};

    TEXTURE_SET_PROXY_VALUE(TextureType::ONE_DIM,                   TextureType::PROXY_ONE_DIM)
    TEXTURE_SET_PROXY_VALUE(TextureType::ONE_DIM_ARRAY,             TextureType::PROXY_ONE_DIM_ARRAY)
    TEXTURE_SET_PROXY_VALUE(TextureType::TWO_DIM,                   TextureType::PROXY_TWO_DIM)
    TEXTURE_SET_PROXY_VALUE(TextureType::TWO_DIM_ARRAY,             TextureType::PROXY_TWO_DIM_ARRAY)
    TEXTURE_SET_PROXY_VALUE(TextureType::TWO_DIM_MULTISAMPLE,       TextureType::PROXY_TWO_DIM_MULTISAMPLE)
    TEXTURE_SET_PROXY_VALUE(TextureType::TWO_DIM_MULTISAMPLE_ARRAY, TextureType::PROXY_TWO_DIM_MULTISAMPLE_ARRAY)
    TEXTURE_SET_PROXY_VALUE(TextureType::THREE_DIM,                 TextureType::PROXY_THREE_DIM)
    TEXTURE_SET_PROXY_VALUE(TextureType::CUBE_MAP,                  TextureType::PROXY_CUBE_MAP)
    TEXTURE_SET_PROXY_VALUE(TextureType::CUBE_MAP_ARRAY,            TextureType::PROXY_CUBE_MAP_ARRAY)
    TEXTURE_SET_PROXY_VALUE(TextureType::RECTANGLE,                 TextureType::PROXY_RECTANGLE)

    enum class CubeTextureDirection : GLenum
    {
        POSITIVE_X = GL_TEXTURE_CUBE_MAP_POSITIVE_X,
        NEGATIVE_X = GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
        POSITIVE_Y = GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
        NEGATIVE_Y = GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
        POSITIVE_Z = GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
        NEGATIVE_Z = GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
    };

    template<TextureType ttype> concept image2dim_condition = 
        ttype == TextureType::TWO_DIM               || 
        ttype == TextureType::PROXY_TWO_DIM         ||
        ttype == TextureType::ONE_DIM_ARRAY         ||
        ttype == TextureType::PROXY_ONE_DIM_ARRAY   ||
        ttype == TextureType::RECTANGLE             ||
        ttype == TextureType::PROXY_RECTANGLE;

    template<TextureType ttype> concept cube_image2dim_condition =
        ttype == TextureType::CUBE_MAP;

    // PName introduction see 
    // https://registry.khronos.org/OpenGL-Refpages/gl4/html/glTexParameter.xhtml
    enum class TexturePName : GLenum
    {
        // TexturePNameValueDepthStencilTextureMode
        DEPTH_STENCIL_TEXTURE_MODE  = GL_DEPTH_STENCIL_TEXTURE_MODE,
        // int, This is an integer value. The initial value is 0.
        BASE_LEVEL                  = GL_TEXTURE_BASE_LEVEL,
        // float[4]
        BORDER_COLOR                = GL_TEXTURE_BORDER_COLOR,
        // PNAME_VALUE_COMPARE_FUNC
        COMPARE_FUNC                = GL_TEXTURE_COMPARE_FUNC,
        // PNAME_VALUE_COMPARE_MODE
        COMPARE_MODE                = GL_TEXTURE_COMPARE_MODE,
        // int
        LOD_BIAS                    = GL_TEXTURE_LOD_BIAS,
        // PNAME_VALUE_MIN_FILTER
        MIN_FILTER                  = GL_TEXTURE_MIN_FILTER,
        // TexturePNameValueMagFilter
        MAG_FILTER                  = GL_TEXTURE_MAG_FILTER,
        // float
        // -1000
        MIN_LOD                     = GL_TEXTURE_MIN_LOD,
        // float
        // 1000
        MAX_LOD                     = GL_TEXTURE_MAX_LOD,
        // int
        // 1000
        MAX_LEVEL                   = GL_TEXTURE_MAX_LEVEL,
        // TexturePNameValueSwizzle
        // The initial value is GL_RED
        SWIZZLE_R                   = GL_TEXTURE_SWIZZLE_R,
        // TexturePNameValueSwizzle
        // The initial value is GL_GREEN
        SWIZZLE_G                   = GL_TEXTURE_SWIZZLE_G,
        // TexturePNameValueSwizzle
        // The initial value is GL_BLUE
        SWIZZLE_B                   = GL_TEXTURE_SWIZZLE_B,
        // TexturePNameValueSwizzle
        // The initial value is GL_ALPHA
        SWIZZLE_A                   = GL_TEXTURE_SWIZZLE_A,
        // TexturePNameValueSwizzle
        SWIZZLE_RGBA                = GL_TEXTURE_SWIZZLE_RGBA,
        // TexturePNameValueWrap
        // GL_REPEAT
        WRAP_S                      = GL_TEXTURE_WRAP_S,
        // TexturePNameValueWrap
        // GL_REPEAT
        WRAP_T                      = GL_TEXTURE_WRAP_T,
        // TexturePNameValueWrap
        // GL_REPEAT
        WRAP_R                      = GL_TEXTURE_WRAP_R
    };

    enum class TextureGetLevelPName : GLenum
    {
        WIDTH                       = GL_TEXTURE_WIDTH,
        HEIGHT                      = GL_TEXTURE_HEIGHT,
        DEPTH                       = GL_TEXTURE_DEPTH,
        INTERNAL_FORMAT             = GL_TEXTURE_INTERNAL_FORMAT,
        RED_SIZE                    = GL_TEXTURE_RED_SIZE,
        GREEN_SIZE                  = GL_TEXTURE_GREEN_SIZE,
        BLUE_SIZE                   = GL_TEXTURE_BLUE_SIZE,
        ALPHA_SIZE                  = GL_TEXTURE_ALPHA_SIZE,
        DEPTH_SIZE                  = GL_TEXTURE_DEPTH_SIZE,
        COMPRESSED                  = GL_TEXTURE_COMPRESSED,
        COMPRESSED_IMAGE_SIZE       = GL_TEXTURE_COMPRESSED_IMAGE_SIZE,
        BUFFER_OFFSE                = GL_TEXTURE_BUFFER_OFFSET
    };

    enum class TexturePNameValueDepthStencilTextureMode
    {
        // then reads from depth-stencil format 
        // textures will return the depth component of the 
        // texel in Rt and the stencil component will be discarded
        DEPTH_COMPONENT             = GL_DEPTH_COMPONENT,
        STENCIL_INDEX               = GL_STENCIL_INDEX
    };

    enum class TexturePNameValueCompareMode
    {
        COMPARE_REF_TO_TEXTURE      = GL_COMPARE_REF_TO_TEXTURE,
        NONE                        = GL_NONE
    };

    enum class TexturePNameValueMinFilter : GLuint
    {
        // Returns the value of the texture element that is nearest (in Manhattan distance) to the specified texture coordinates.
        NEAREST                     = GL_NEAREST,
        // Returns the weighted average of the four texture elements that are closest to the specified texture coordinates. 
        // These can include items wrapped or repeated from other parts of a texture, 
        // depending on the values of GL_TEXTURE_WRAP_S and GL_TEXTURE_WRAP_T, and on the exact mapping.
        LINEAR                      = GL_LINEAR,
        // Chooses the mipmap that most closely matches the size of the pixel being textured and uses 
        // the GL_NEAREST criterion (the texture element closest to the specified texture coordinates) to produce a texture value.
        NEAREST_MIPMAP_NEAREST      = GL_NEAREST_MIPMAP_NEAREST,
        // Chooses the mipmap that most closely matches the size of the pixel being textured and uses the GL_LINEAR criterion 
        // (a weighted average of the four texture elements that are closest to the specified texture coordinates) to produce a texture value.
        LINEAR_MIPMAP_NEAREST       = GL_LINEAR_MIPMAP_NEAREST,
        // Chooses the two mipmaps that most closely match the size of the pixel being textured and uses the GL_NEAREST criterion 
        // (the texture element closest to the specified texture coordinates ) to produce a texture value from each mipmap. 
        // The final texture value is a weighted average of those two values.
        NEAREST_MIPMAP_LINEAR       = GL_NEAREST_MIPMAP_LINEAR,
        // Chooses the two mipmaps that most closely match the size of the pixel being textured and uses the GL_LINEAR criterion 
        // (a weighted average of the texture elements that are closest to the specified texture coordinates) to produce a texture 
        // value from each mipmap. The final texture value is a weighted average of those two values.
        LINEAR_MIPMAP_LINEA         = GL_LINEAR_MIPMAP_LINEAR
    };

    // GL_NEAREST is generally faster than GL_LINEAR, but it can produce textured images with sharper edges because the transition 
    // between texture elements is not as smooth. The initial value of GL_TEXTURE_MAG_FILTER is GL_LINEAR.
    enum class TexturePNameValueMagFilter : GLuint
    {
        // Returns the value of the texture element that is nearest (in Manhattan distance) to the specified texture coordinates.
        NEAREST                     = GL_NEAREST,
        // Returns the weighted average of the texture elements that are closest to the specified texture coordinates. 
        // These can include items wrapped or repeated from other parts of a texture, 
        // depending on the values of GL_TEXTURE_WRAP_S and GL_TEXTURE_WRAP_T, and on the exact mapping.
        LINEAR                      = GL_LINEAR 
    };

    enum class TexturePNameValueSwizzle : GLuint
    {
        RED                         = GL_RED,
        GREEN                       = GL_GREEN,
        BLUE                        = GL_BLUE,
        ALPHA                       = GL_ALPHA,
        ZERO                        = GL_ZERO,
        ONE                         = GL_ONE
    };

    enum class TexturePNameValueWrap : GLuint
    {
        CLAMP_TO_EDGE               = GL_CLAMP_TO_EDGE,
        CLAMP_TO_BORDER             = GL_CLAMP_TO_BORDER,
        MIRRORED_REPEAT             = GL_MIRRORED_REPEAT,
        REPEAT                      = GL_REPEAT,
        MIRROR_CLAMP_TO_EDGE        = GL_MIRROR_CLAMP_TO_EDGE
    };

/**
 * concept and type map for glGetTexLevelParameterfv glGetTextureLevelParameterfv
*/
    template<class T> struct TextureGetLevelParameterTypeMap;
#define TEXTURE_GET_LEVEL_PARAMETER_TYPE_MAP(SUFFIX, PARAM_TYPE)\
    template<> struct TextureGetLevelParameterTypeMap<PARAM_TYPE>{\
        constexpr static auto& tex_method     = glGetTexLevelParameter ## SUFFIX ;\
        constexpr static auto& texture_method = glGetTextureLevelParameter ## SUFFIX ;\
        using value_type = PARAM_TYPE;\
    };
    
    TEXTURE_GET_LEVEL_PARAMETER_TYPE_MAP(fv, GLfloat)
    TEXTURE_GET_LEVEL_PARAMETER_TYPE_MAP(iv, GLint)

    template<class _ttype> concept level_param_allowed = requires{
        std::is_same_v<_ttype, TextureType> || 
        std::is_same_v<_ttype, CubeTextureDirection>;
    };

    template<class T> struct TextureParameterTypeMap;
#define TEXTURE_SET_PARAMETER_TYPE_MAP(SUFFIX, TYPE, TRUE_TYPE)\
    template<> struct TextureParameterTypeMap<TYPE>{\
        constexpr static auto& set_tex_method     = glTexParameter ## SUFFIX ;\
        constexpr static auto& set_texture_method = glTextureParameter ## SUFFIX ;\
        using value_type = TYPE;\
        using true_type = TRUE_TYPE;\
    };

    TEXTURE_SET_PARAMETER_TYPE_MAP(i,  TexturePNameValueDepthStencilTextureMode,  GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(i,  CompareFunc,                               GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(i,  TexturePNameValueCompareMode,              GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(i,  TexturePNameValueMinFilter,                GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(i,  TexturePNameValueMagFilter,                GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(i,  TexturePNameValueSwizzle,                  GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(i,  TexturePNameValueWrap,                     GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(i,  GLint,                                     GLint)
    TEXTURE_SET_PARAMETER_TYPE_MAP(f,  GLfloat,                                   GLfloat)
    TEXTURE_SET_PARAMETER_TYPE_MAP(fv, GLfloat*,                                  GLfloat*)

    template<TexturePName> struct TexPNameValueType;
#define TEXTURE_PNAME_VALUE_TYPE_MAP(pname, value_type)\
    template<> struct TexPNameValueType<pname>{\
        using type = value_type;\
    };

    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::DEPTH_STENCIL_TEXTURE_MODE,   TexturePNameValueDepthStencilTextureMode)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::BASE_LEVEL,                   GLint)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::BORDER_COLOR,                 GLfloat(&)[4])
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::COMPARE_FUNC,                 CompareFunc)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::COMPARE_MODE,                 TexturePNameValueCompareMode)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::LOD_BIAS,                     GLfloat)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::MIN_FILTER,                   TexturePNameValueMinFilter)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::MAG_FILTER,                   TexturePNameValueMagFilter)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::MIN_LOD,                      GLfloat)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::MAX_LOD,                      GLfloat)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::MAX_LEVEL,                    GLint)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::SWIZZLE_R,                    TexturePNameValueSwizzle)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::SWIZZLE_G,                    TexturePNameValueSwizzle)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::SWIZZLE_B,                    TexturePNameValueSwizzle)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::SWIZZLE_A,                    TexturePNameValueSwizzle)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::SWIZZLE_RGBA,                 TexturePNameValueSwizzle)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::WRAP_S,                       TexturePNameValueWrap)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::WRAP_T,                       TexturePNameValueWrap)
    TEXTURE_PNAME_VALUE_TYPE_MAP(TexturePName::WRAP_R,                       TexturePNameValueWrap)

    // https://registry.khronos.org/OpenGL-Refpages/gl4/html/glTexParameter.xhtml
    template<TextureType _ttype> concept param_allowed = 
        _ttype == TextureType::ONE_DIM                      ||
        _ttype == TextureType::ONE_DIM_ARRAY                ||
        _ttype == TextureType::TWO_DIM                      ||
        _ttype == TextureType::TWO_DIM_ARRAY                ||
        _ttype == TextureType::TWO_DIM_MULTISAMPLE          ||
        _ttype == TextureType::TWO_DIM_MULTISAMPLE_ARRAY    ||
        _ttype == TextureType::THREE_DIM                    ||
        _ttype == TextureType::CUBE_MAP                     ||
        _ttype == TextureType::CUBE_MAP_ARRAY               ||
        _ttype == TextureType::RECTANGLE;
}
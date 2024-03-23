#pragma once
#include <glad/glad.h>
#include <type_traits>
#include "ValueTypes.hpp"

namespace lux::gapi::opengl{
    enum class TextureType : GLenum
    {
        // One-dimensional textures are primarily used for 
        // storing one-dimensional data (like gradients) or 
        // when the texture's height is not important.
        ONE_DIM                     = GL_TEXTURE_1D,
        // One-dimensional texture arrays can store multiple 
        // one-dimensional textures, used for various effects 
        // like material transitions, data charts, etc.
        ONE_DIM_ARRAY               = GL_TEXTURE_1D_ARRAY,
        // Three-dimensional textures are used for storing 
        // volumetric data, common in volume rendering, 
        // 3D texture mapping, etc.
        THREE_DIM                   = GL_TEXTURE_3D,
        // Cube maps are used for environment mapping to simulate 
        // reflections or refractions, commonly used in creating 
        // skyboxes or simulating reflections on smooth surfaces.
        CUBE_MAP                    = GL_TEXTURE_CUBE_MAP,
        // Cube map arrays are collections of cube maps, 
        // which can be used to implement objects with 
        // various reflection or refraction effects.
        CUBE_MAP_ARRAY              = GL_TEXTURE_CUBE_MAP_ARRAY,
        // Two-dimensional textures are the most commonly used texture type, 
        // used for texture mapping on various surfaces like walls, floors, etc.
        TWO_DIM                     = GL_TEXTURE_2D,
        // Two-dimensional texture arrays can store multiple two-dimensional 
        // textures, commonly used for implementing character sets, 
        // layered terrain textures, etc.
        TWO_DIM_ARRAY               = GL_TEXTURE_2D_ARRAY,
        // Multisample two-dimensional textures are used for anti-aliasing 
        // effects, directly used in the rendering to texture process.
        TWO_DIM_MULTISAMPLE         = GL_TEXTURE_2D_MULTISAMPLE,
        // Multisample two-dimensional texture arrays are the array form of 
        // multisample two-dimensional textures, used in advanced anti-aliasing 
        // techniques like multisample anti-aliasing (MSAA) in deferred rendering.
        TWO_DIM_MULTISAMPLE_ARRAY   = GL_TEXTURE_2D_MULTISAMPLE_ARRAY,
        // Rectangle textures are mainly used in specific non-power of two 
        // texture scenarios, like video playback or UI elements rendering.

        // Non - Power - Of - Two(NPOT) textures refer to textures whose width 
        // and height are not powers of two.In earlier graphics APIs, texture 
        // dimensions were typically required to be powers of two(e.g., 2, 4, 8, 
        // 16, 32, 64, etc.), due to hardware and driver limitations, allowing 
        // such textures to be processed and mapped more efficiently.However, 
        // this restriction also meant that if a texture's natural size did not 
        // conform to this requirement, it had to be resized or padded, potentially 
        // leading to wasted memory and reduced performance.
        // With advancements in hardware, modern graphics APIs(such as OpenGL ES 2.0 
        // and above, DirectX 10 and above, and Vulkan) now support NPOT textures, 
        // enhancing the flexibility and efficiency of texture usage.Developers can 
        // use images in their original sizes as textures without the need for resizing 
        // or padding, which is beneficial for optimizing memory use and performance.
        RECTANGLE                   = GL_TEXTURE_RECTANGLE,
        // Texture buffers are used to directly use buffer data as a texture, 
        // commonly used in advanced shading techniques, like deformable animation 
        // or data visualization.
        BUFFER                      = GL_TEXTURE_BUFFER, // no proxy

        // proxy
        // Proxy textures are mainly used to query certain parameters at texture 
        // creation time, such as the maximum supported size and format, without 
        // actually creating the texture. 
        // These types are typically used for pre-checking textures.
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


    enum class PixelStoreParameter
    {                                                   // type     initial value   valid range
        PACK_SWAP_BYTES     = GL_PACK_SWAP_BYTES,       // bool     false           true or false
        PACK_LSB_FIRST      = GL_PACK_LSB_FIRST,        // bool     false           true or false
        PACK_ROW_LENGTH     = GL_PACK_ROW_LENGTH,       // int      0               [0, inf)
        PACK_IMAGE_HEIGHT   = GL_PACK_IMAGE_HEIGHT,     // int      0               [0, inf)
        PACK_SKIP_ROWS      = GL_PACK_SKIP_ROWS,        // int      0               [0, inf)
        PACK_SKIP_PIXELS    = GL_PACK_SKIP_PIXELS,      // int      0               [0, inf)
        PACK_SKIP_IMAGES    = GL_PACK_SKIP_IMAGES,      // int      0               [0, inf)
        PACK_ALIGNMENT      = GL_PACK_ALIGNMENT,        // int      4               [1, 2, 4, 8]
        UNPACK_SWAP_BYTES   = GL_UNPACK_SWAP_BYTES,     // bool     false           true or false
        UNPACK_LSB_FIRST    = GL_UNPACK_LSB_FIRST,      // bool     false           true or false
        UNPACK_ROW_LENGTH   = GL_UNPACK_ROW_LENGTH,     // int      0               [0, inf)
        UNPACK_IMAGE_HEIGHT = GL_UNPACK_IMAGE_HEIGHT,   // int      0               [0, inf)
        UNPACK_SKIP_ROWS    = GL_UNPACK_SKIP_ROWS,      // int      0               [0, inf)
        UNPACK_SKIP_PIXELS  = GL_UNPACK_SKIP_PIXELS,    // int      0               [0, inf)
        UNPACK_SKIP_IMAGES  = GL_UNPACK_SKIP_IMAGES,    // int      0               [0, inf)
        UNPACK_ALIGNMENT    = GL_UNPACK_ALIGNMENT       // int      4               [1, 2, 4, 8]
    };

    template<PixelStoreParameter> struct PixStoreTypeMap { using type = GLint; };
    template<> struct PixStoreTypeMap<PixelStoreParameter::PACK_SWAP_BYTES> { using type = GLboolean; };
    template<> struct PixStoreTypeMap<PixelStoreParameter::PACK_LSB_FIRST> { using type = GLboolean; };
    template<> struct PixStoreTypeMap<PixelStoreParameter::UNPACK_SWAP_BYTES> { using type = GLboolean; };
    template<> struct PixStoreTypeMap<PixelStoreParameter::UNPACK_LSB_FIRST> { using type = GLboolean; };

    enum class UnpackAlignment
    {
        BYTE_ALIGNMENT = 1,
        ROW_ALIGNMENT  = 2,
        WORD_ALIGNMENT = 4,
        DOUBLE_WORD    = 8
    };
}
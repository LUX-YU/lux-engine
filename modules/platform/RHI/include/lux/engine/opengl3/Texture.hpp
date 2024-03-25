#pragma once
#include "TextureType.hpp"
#include "ValueTypes.hpp"

namespace lux::gapi::opengl{
    class TextureBase
    {
    public:
        ~TextureBase()
        {
            release();
        }

        TextureBase(const TextureBase&) = delete;
        TextureBase& operator=(const TextureBase&) = delete;

        TextureBase(TextureBase&& other) noexcept
        {
            _num = other._num;
            _textures = other._textures;
            other._num = 0;
        }

        TextureBase& operator=(TextureBase&& other) noexcept
        {
            release();
            _num = other._num;
            _textures = other._textures;
            other._num = 0;
            return *this;
        }

        GLsizei number() const
        {
            return _num;
        }

        void release()
        {
            if(_num > 0){
                glDeleteTextures(_num, &_textures);
                _num = 0;
            }
        }

        GLuint rawObject()
        {
            return _textures;
        }

        // for glad, index is less than 31
        static void sActiveTexture(GLenum index)
        {
            glActiveTexture(GL_TEXTURE0 + index);
        }

    protected:

        TextureBase(GLsizei num)
        {
            _num = num;
            glGenTextures(num, &_textures);
        }

        GLsizei _num;
        GLuint  _textures;
    };

    struct TextureImage
    {
        // mipmap level
        GLint               level;
        /*
            /// Base Internal Format:
            ImageFormat::DEPTH_COMPONENT    ImageFormat::DEPTH_STENCIL
            ImageFormat::RED                ImageFormat::RG
            ImageFormat::RGB                ImageFormat::RGBA

            /// Sized Internal Formats
            /// ImageFormat::RED
            ImageFormat::R8             ImageFormat::R8_SNORM   ImageFormat::R16            ImageFormat::R16_SNORM
            ImageFormat::R16F           ImageFormat::R8I        ImageFormat::R8UI           ImageFormat::R16I
            ImageFormat::R16UI          ImageFormat::R32I       ImageFormat::R32UI
                /// Compressed
            ImageFormat::COMPRESSED_RED                         ImageFormat::COMPRESSED_RED_RGTC1
            ImageFormat::COMPRESSED_SIGNED_RED_RGTC1

            /// ImageFormat::RG
            ImageFormat::RG8            ImageFormat::RG8_SNORM  ImageFormat::RG16           ImageFormat::RG16_SNORM
            ImageFormat::RG16F          ImageFormat::R32F       ImageFormat::RG32F          ImageFormat::RG8I           
            ImageFormat::RG8UI          ImageFormat::RG16I      ImageFormat::RG16UI         ImageFormat::RG32I          
            ImageFormat::RG32UI
                /// Compressed
            ImageFormat::COMPRESSED_RG                          ImageFormat::COMPRESSED_RG_RGTC2
            ImageFormat::COMPRESSED_SIGNED_RG_RGTC2

            /// ImageFormat::RGB
            ImageFormat::R3_G3_B2       ImageFormat::RGB4       ImageFormat::RGB5           ImageFormat::RGB8           
            ImageFormat::RGB8_SNORM     ImageFormat::RGB10      ImageFormat::RGB12          ImageFormat::RGB16_SNORM
            ImageFormat::RGBA2          ImageFormat::RGBA4      ImageFormat::SRGB8          ImageFormat::RGB16F
            ImageFormat::RGB32F         ImageFormat::R11F_G11F_B10F                         ImageFormat::RGB9_E5
            ImageFormat::RGB8I          ImageFormat::RGB8UI     ImageFormat::RGB16I         ImageFormat::RGB16UI
            ImageFormat::RGB32I         ImageFormat::RGB32UI
                /// Compressed
            ImageFormat::COMPRESSED_RGB                         ImageFormat::COMPRESSED_SRGB
            ImageFormat::COMPRESSED_RGB_BPTC_SIGNED_FLOAT       ImageFormat::COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT

            /// ImageFormat::RGBA
            ImageFormat::RGB5_A1        ImageFormat::RGBA8      ImageFormat::RGBA8_SNORM    ImageFormat::RGB10_A2
            ImageFormat::RGB10_A2UI     ImageFormat::RGBA12     ImageFormat::RGBA16         ImageFormat::SRGB8_ALPHA8
            ImageFormat::RGBA16F        ImageFormat::RGBA32F    ImageFormat::RGBA8I         ImageFormat::RGBA8UI
            ImageFormat::RGBA16I        ImageFormat::RGBA16UI   ImageFormat::RGBA32I        ImageFormat::RGBA32U
                /// Compressed
            ImageFormat::COMPRESSED_RGBA                        ImageFormat::COMPRESSED_SRGB_ALPHA
            ImageFormat::COMPRESSED_RGBA_BPTC_UNORM             ImageFormat::COMPRESSED_SRGB_ALPHA_BPTC_UNORM
        */
        // This parameter specifies the format in which the texture is stored in the GPU.
        ImageFormat         internalformat;
        GLsizei             width;
        GLsizei             height;
        /*
            ImageFormat::RED,             ImageFormat::RG
            ImageFormat::RGB,             ImageFormat::BGR
            ImageFormat::RGBA,            ImageFormat::BGRA
            ImageFormat::DEPTH_COMPONENT, ImageFormat::DEPTH_STENCIL
        */
        // This parameter describes the format of the data you are passing to OpenGL.
        ImageFormat         format;
        /*
            DataType::UNSIGNED_BYTE,              DataType::BYTE,
            DataType::UNSIGNED_SHORT,             DataType::SHORT,
            DataType::UNSIGNED_INT,               DataType::INT,
            DataType::HALF_FLOAT,                 DataType::FLOAT,
            DataType::UNSIGNED_BYTE_3_3_2,        DataType::UNSIGNED_BYTE_2_3_3_REV,
            DataType::UNSIGNED_SHORT_5_6_5,       DataType::UNSIGNED_SHORT_5_6_5_REV,
            DataType::UNSIGNED_SHORT_4_4_4_4,     DataType::UNSIGNED_SHORT_4_4_4_4_REV,
            DataType::UNSIGNED_SHORT_5_5_5_1,     DataType::UNSIGNED_SHORT_1_5_5_5_REV,
            DataType::UNSIGNED_INT_8_8_8_8,       DataType::UNSIGNED_INT_8_8_8_8_REV,
            DataType::UNSIGNED_INT_10_10_10_2,    DataType::UNSIGNED_INT_2_10_10_10_REV.
        */
        DataType            type;
        void*               data;
    };

    template<TextureType TexType>
    class TTextureBase : public TextureBase
    {
    public:
        static constexpr auto gl_texture_type = static_cast<GLenum>(TexType);

        TTextureBase(GLsizei size) : TextureBase(size){}
        TTextureBase() : TextureBase(1){}

        void bind()
        requires bindable<TexType>
        {
            // glBindTexture lets you create or use a named texture
            // When a texture is bound to a target, the previous binding for that target is automatically broken.

            // The target parameter of glBindTexture corresponds to the texture's type. 
            // So when you use a freshly generated texture name, 
            // the first bind helps define the type of the texture. 
            // It is not legal to bind an object to a different target than the one 
            // it was previously bound with
            glBindTexture(gl_texture_type, _textures);
        }

        void bindRange(GLuint first, GLuint count)
        requires bindable<TexType>
        {
            glBindTextures(first, count, &_textures);
        }

        static void endBind()
        requires bindable<TexType>
        {
            glBindTexture(gl_texture_type, 0);
        }

        /** 
         * @brief static member function get level parameter
         * 
         * @param level Specifies the level-of-detail number of the desired image. 
         *              Level 0 is the base image level. Level n is the nth mipmap reduction image.
         * @param pname see TextureGetLevelPName declaration
         * @param out   GLint or GLfloat
        */
        template<class T>
        static void sGetLevelParameter(GLint level, TextureGetLevelPName pname, T* out)
        requires level_param_allowed<TexType>
        {
            TextureGetLevelParameterTypeMap<T>::tex_method(gl_texture_type, level, static_cast<GLenum>(pname), out);
        }
        template<TextureGetLevelPName PName, class ValueType = GLint>
        static void sTGetLevelParameter(GLint level, ValueType* out)
        requires level_param_allowed<TexType>
        {
            sGetLevelParameter(gl_texture_type, level, PName, out);
        }

        /**
         * @brief member function get level parameter
         * 
         * @param level Specifies the level-of-detail number of the desired image. 
         *              Level 0 is the base image level. Level n is the nth mipmap reduction image.
         * @param pname see TextureGetLevelPName declaration
         * @param out   GLint or GLfloat
        */
        template<class T>
        void getLevelParameter(GLint level, TextureGetLevelPName pname, T* out)
        requires level_param_allowed<TexType>
        {
            TextureGetLevelParameterTypeMap<T>::texture_method(_textures, level, static_cast<GLenum>(pname), out);
        }
        template<TextureGetLevelPName PName, class ValueType = GLint>
        void TGetLevelParameter(GLint level, ValueType* out)
        requires level_param_allowed<TexType>
        {
            getLevelParameter(level, PName, out);
        }

        // Call after binding
        template<class T>
        static void sSetTexParameter(TexturePName pname, T value)
        requires param_allowed<TexType>
        {
            using value_type = typename TextureParameterTypeMap<T>::true_type;
            TextureParameterTypeMap<T>::set_tex_method(gl_texture_type, static_cast<GLenum>(pname), static_cast<value_type>(value));
        }
        template<TexturePName PName, class ValueType = typename TexPNameValueType<PName>::type>
        static void sTSetTexParameter(ValueType value)
        requires param_allowed<TexType>
        {
            sSetTexParameter(PName, value);
        }

        // textureParameter, be allowed after opengl 4.5
        template<class T>
        void setTextureParameter(TexturePName pname, T value)
        requires param_allowed<TexType>
        {
            using value_type = typename TextureParameterTypeMap<T>::true_type;
            TextureParameterTypeMap<T>::set_texture_method(
                _textures, 
                static_cast<GLenum>(pname), 
                static_cast<value_type>(value)
            );
        }
        template<TexturePName PName, class ValueType = typename TexPNameValueType<PName>::type>
        void TSetTextureParameter(ValueType value)
        requires param_allowed<TexType>
        {
            setTextureParameter(PName, value);
        }

        /**
         *  @brief static member function
         *  
         *  @param image image parameter, see declaration of TextureImage
         **/ 
        static void sImage2D(TextureImage& image)
        requires image2dim_condition<TexType>
        {
            glTexImage2D(gl_texture_type, image.level, static_cast<GLint>(image.internalformat), image.width, image.height, 
                0, static_cast<GLenum>(image.format), static_cast<GLenum>(image.type), image.data);
        }

        /**
         *  @brief static member function , param description see declaration of TextureImage
         **/ 
        static void sImage2DRaw(GLint level, ImageFormat internalformat, 
            GLsizei width, GLsizei height, GLint border, ImageFormat format, DataType type, const void *pixels)
        requires image2dim_condition<TexType>
        {
            glTexImage2D(gl_texture_type, level, static_cast<GLint>(internalformat), 
                width, height, border, static_cast<GLenum>(format), static_cast<GLenum>(type), pixels);
        }

        /**
         *  @brief static member function, this function is available only TextureType == CUBE_MAP
         *  
         *  @param direction cube image direction
         *  @param image image parameter
         **/ 
        static void sImage2D(CubeTextureDirection direction, TextureImage& image)
        requires cube_image2dim_condition<TexType>
        {
            glTexImage2D(static_cast<GLenum>(direction), image.level, static_cast<GLint>(image.internalformat), image.width, image.height, 
                0, static_cast<GLenum>(image.format), static_cast<GLenum>(image.type), image.data);
        }

        /**
         *  @brief static member function
         *  
         *  @param image image parameter
         **/ 
        static void sImage2DRaw(CubeTextureDirection direction, GLint level, ImageFormat internalformat, 
            GLsizei width, GLsizei height, GLint border, ImageFormat format, DataType type, const void *pixels)
        requires cube_image2dim_condition<TexType>
        {
            glTexImage2D(static_cast<GLenum>(direction), level, static_cast<GLint>(internalformat), 
                width, height, border, static_cast<GLenum>(format), static_cast<GLenum>(type), pixels);
        }

        static void sGenerateMipmap()
        requires texmipmap<TexType>
        {
            glGenerateMipmap(gl_texture_type);
        }

        template<PixelStoreParameter E, class ValueType = typename PixStoreTypeMap<E>::type>
        static void sTPixelStore(ValueType value)
        {
            glPixelStore(static_cast<std::underlying_type_t<PixelStoreParameter>>(E), value);
        }
    };

    using Texture1D     = TTextureBase<TextureType::ONE_DIM>;
    using Texture2D     = TTextureBase<TextureType::TWO_DIM>;
    using CubeTexture   = TTextureBase<TextureType::CUBE_MAP>;
}

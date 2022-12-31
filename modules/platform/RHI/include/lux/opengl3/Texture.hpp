#pragma once
#include "TextureType.hpp"

namespace lux::gapiwrap::opengl{
    class TextureBase
    {
    public:
        inline GLsizei number() const
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

        TextureBase(const TextureBase&) = delete;
        TextureBase& operator=(const TextureBase&) = delete;

        TextureBase(TextureBase&& other)
        {
            _num = other._num;
            _textures = other._textures;
            other._num = 0;
        }

        TextureBase& operator=(TextureBase&& other)
        {
            _num = other._num;
            _textures = other._textures;
            other._num = 0;
            return *this;
        }

        inline void bindRange(GLuint first, GLuint count)
        {
            glBindTextures(first, count, &_textures);
        }

        ~TextureBase()
        {
            if(_num > 0) 
                glDeleteTextures(_num, &_textures);
        }

        GLuint rawObject()
        {
            return _textures;
        }

    protected:

        enum class ImageTextureType
        {
            TEXTURE_2D                  = GL_TEXTURE_2D, 
            PROXY_TEXTURE_2D            = GL_PROXY_TEXTURE_2D, 
            TEXTURE_1D_ARRAY            = GL_TEXTURE_1D_ARRAY, 
            PROXY_TEXTURE_1D_ARRAY      = GL_PROXY_TEXTURE_1D_ARRAY, 
            TEXTURE_RECTANGLE           = GL_TEXTURE_RECTANGLE, 
            PROXY_TEXTURE_RECTANGLE     = GL_PROXY_TEXTURE_RECTANGLE, 
            TEXTURE_CUBE_MAP_POSITIVE_X = GL_TEXTURE_CUBE_MAP_POSITIVE_X, 
            TEXTURE_CUBE_MAP_NEGATIVE_X = GL_TEXTURE_CUBE_MAP_NEGATIVE_X, 
            TEXTURE_CUBE_MAP_POSITIVE_Y = GL_TEXTURE_CUBE_MAP_POSITIVE_Y, 
            TEXTURE_CUBE_MAP_NEGATIVE_Y = GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, 
            TEXTURE_CUBE_MAP_POSITIVE_Z = GL_TEXTURE_CUBE_MAP_POSITIVE_Z, 
            TEXTURE_CUBE_MAP_NEGATIVE_Z = GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 
            PROXY_TEXTURE_CUBE_MAP      = GL_PROXY_TEXTURE_CUBE_MAP
        };

        static constexpr inline void image(ImageTextureType target, GLint level, GLint internalformat, 
            GLsizei width, GLsizei height, GLenum format, GLenum type, const void * data)
        {
            glTexImage2D( 
                static_cast<GLenum>(target), 
                level, 
                internalformat, width, height, 
                /*border must be 0*/ 0, 
                format, 
                type, 
                data
            );
        }

        TextureBase(GLsizei num)
        {
            _num = num;
            glGenTextures(num, &_textures);
        }

        GLsizei _num;
        GLuint  _textures;
    };

    template<TextureType TexType>
    class TTextureBase : public TextureBase
    {
    public:
        static constexpr auto gl_texture_type = static_cast<GLenum>(TexType);

        TTextureBase(GLsizei size) : TextureBase(size){}

        inline void bind()
        {
            glBindTexture(gl_texture_type, _textures);
        }

        static inline void unbind()
        {
            glBindTexture(gl_texture_type, 0);
        }

        // Call after binding
        template<class T>
        static inline void staticTexParmeter(TexturePName pname, T value)
        requires parameter_condition<TexType>
        {
            using value_type = typename TextureSetParameterTypeMap<T>::true_type;
            TextureSetParameterTypeMap<T>::tex_method(
                gl_texture_type, 
                static_cast<GLenum>(pname), 
                static_cast<value_type>(value)
            );
        }

        // pname is specified in compile time
        template<TexturePName PNAME, class ValueType = typename TexPNameValueType<PNAME>::type>
        requires parameter_condition<TexType>
        static constexpr inline void TStaticTexParmeter(ValueType value)
        {
            using value_type = typename TextureSetParameterTypeMap<ValueType>::true_type;
            TextureSetParameterTypeMap<ValueType>::tex_method(
                gl_texture_type, 
                static_cast<GLenum>(PNAME),
                static_cast<value_type>(value)
            );
        }

        // textureParameter, be allowed after opengl 4.5
        template<class T>
        inline void textureParmeter(TexturePName pname, T value)
        requires parameter_condition<TexType>
        {
            using value_type = typename TextureSetParameterTypeMap<T>::true_type;
            TextureSetParameterTypeMap<T>::texture_method(
                _textures, 
                static_cast<GLenum>(pname), 
                static_cast<value_type>(value)
            );
        }

        // pname is specified in compile time
        template<TexturePName PNAME, class ValueType = typename TexPNameValueType<PNAME>::type>
        requires parameter_condition<TexType>
        inline constexpr void TTextureParmeter(ValueType value)
        {
            using value_type = typename TextureSetParameterTypeMap<ValueType>::true_type;
            TextureSetParameterTypeMap<ValueType>::texture_method(
                _textures, 
                static_cast<GLenum>(PNAME), 
                static_cast<value_type>(value)
            );
        }
    };

    class Texture2D : public TTextureBase<TextureType::TEXTURE_2D>
    {
    public:
        Texture2D(GLsizei size) : TTextureBase(size){}

        static constexpr inline void image(GLint level, GLint internalformat, 
            GLsizei width, GLsizei height, GLenum format, GLenum type, const void * data)
        {
            TTextureBase::image(
                ImageTextureType::TEXTURE_2D, 
                level, 
                internalformat, width, height,
                format, 
                type, 
                data
            );
        }
    };

    class CubeMapTexture : public TTextureBase<TextureType::TEXTURE_CUBE_MAP>
    {
    public:
        CubeMapTexture(GLsizei size) : TTextureBase(size){}

        enum class Direction
        {
            POSITIVE_X = GL_TEXTURE_CUBE_MAP_POSITIVE_X, 
            NEGATIVE_X = GL_TEXTURE_CUBE_MAP_NEGATIVE_X, 
            POSITIVE_Y = GL_TEXTURE_CUBE_MAP_POSITIVE_Y, 
            NEGATIVE_Y = GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, 
            POSITIVE_Z = GL_TEXTURE_CUBE_MAP_POSITIVE_Z, 
            NEGATIVE_Z = GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
        };

        static constexpr inline void image(Direction direction, GLint level, GLint internalformat, 
            GLsizei width, GLsizei height, GLenum format, GLenum type, const void * data)
        {
            TTextureBase::image(
                static_cast<ImageTextureType>(direction),
                level, 
                internalformat, 
                width, height,
                format, 
                type, 
                data
            );
        }
    };
}
#pragma once
#include <glad/glad.h>
#include "config.h"
#include "Texture.hpp"
#include "RenderBuffer.hpp"

// @ref https://registry.khronos.org/OpenGL-Refpages/gl4/html/glFramebufferTexture.xhtml

/**
 * @ref 
 *
 * ## 9.2.8.1 Effects of Attaching a Texture Image
 * The remaining comments in this section apply to all forms of *FramebufferTexture*.
 * If texture is zero, any image or array of images attached to the attachment point
 * named by attachment is detached. Any additional parameters (level, textarget,
 * and/or layer) are ignored when texture is zero. All state values of the attachment
 * point specified by attachment are set to their default values listed in table 23.31.
 * If texture is not zero, and if *FramebufferTexture* is successful, then the
 * specified texture image will be used as the logical buffer identified by attachment
 * of the framebuffer object currently bound to target. State values of the specified
 * attachment point are set as follows:
 * • The value of FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE is set to
 * TEXTURE.
 * • The value of FRAMEBUFFER_ATTACHMENT_OBJECT_NAME is set to texture.
 * • The value of FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL is set to level.
 * • If *FramebufferTexture2D is called and texture is a cube map texture, then
 * the value of FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE is
 * set to textarget; otherwise it is set to the default value (NONE). 
*/


namespace lux::gapi::opengl
{
    template<TextureType ttype> concept levelzerotex = 
        ttype == TextureType::RECTANGLE ||
        ttype == TextureType::TWO_DIM_MULTISAMPLE || 
        ttype == TextureType::TWO_DIM_MULTISAMPLE_ARRAY;

    enum class AttachmentType
    {
        // MAX Supported color use staticGetMaxColorAttachment
        COLOR0  = GL_COLOR_ATTACHMENT0,
        COLOR1  = GL_COLOR_ATTACHMENT1,
        COLOR2  = GL_COLOR_ATTACHMENT2,
        COLOR3  = GL_COLOR_ATTACHMENT3,
        COLOR4  = GL_COLOR_ATTACHMENT4,
        COLOR5  = GL_COLOR_ATTACHMENT5,
        COLOR6  = GL_COLOR_ATTACHMENT6,
        COLOR7  = GL_COLOR_ATTACHMENT7,
        COLOR8  = GL_COLOR_ATTACHMENT8,
        COLOR9  = GL_COLOR_ATTACHMENT9,
        COLOR10 = GL_COLOR_ATTACHMENT10,
        COLOR11 = GL_COLOR_ATTACHMENT11,
        COLOR12 = GL_COLOR_ATTACHMENT12,
        COLOR13 = GL_COLOR_ATTACHMENT13,
        COLOR14 = GL_COLOR_ATTACHMENT14,
        COLOR15 = GL_COLOR_ATTACHMENT15,
        COLOR16 = GL_COLOR_ATTACHMENT16,
        COLOR17 = GL_COLOR_ATTACHMENT17,
        COLOR18 = GL_COLOR_ATTACHMENT18,
        COLOR19 = GL_COLOR_ATTACHMENT19,
        COLOR20 = GL_COLOR_ATTACHMENT20,
        COLOR21 = GL_COLOR_ATTACHMENT21,
        COLOR22 = GL_COLOR_ATTACHMENT22,
        COLOR23 = GL_COLOR_ATTACHMENT23,
        COLOR24 = GL_COLOR_ATTACHMENT24,
        COLOR25 = GL_COLOR_ATTACHMENT25,
        COLOR26 = GL_COLOR_ATTACHMENT26,
        COLOR27 = GL_COLOR_ATTACHMENT27,
        COLOR28 = GL_COLOR_ATTACHMENT28,
        COLOR29 = GL_COLOR_ATTACHMENT29,
        COLOR30 = GL_COLOR_ATTACHMENT30,
        COLOR31 = GL_COLOR_ATTACHMENT31,
        DEPTH   = GL_DEPTH_ATTACHMENT,
        STENCIL = GL_STENCIL_ATTACHMENT,
        DEPTH_STENCIL = GL_DEPTH_STENCIL_ATTACHMENT
    };

    enum class FBStatus
    {
        FRAMEBUFFER_COMPLETE            = GL_FRAMEBUFFER_COMPLETE,
        UNDEFINED                       = GL_FRAMEBUFFER_UNDEFINED,
        INCOMPLETE_ATTACHMENT           = GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT,
        INCOMPLETE_MISSING_ATTACHMENT   = GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT,
        INCOMPLETE_DRAW_BUFFER          = GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER,
        INCOMPLETE_READ_BUFFER          = GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER,
        UNSUPPORTED                     = GL_FRAMEBUFFER_UNSUPPORTED,
        INCOMPLETE_MULTISAMPLE          = GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE,
        INCOMPLETE_LAYER_TARGETS        = GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS
    };

    enum class ColorBuffer
    {
        NONE        = GL_NONE,
        // The left and right buffers are used for stereoscopic rendering
        FRONT_LEFT  = GL_FRONT_LEFT,
        BACK_LEFT   = GL_BACK_LEFT,
        FRONT_RIGHT = GL_FRONT_RIGHT,
        BACK_RIGHT  = GL_BACK_RIGHT,
        // The default framebuffer's set of 4 color images 
        // have certain aliases that represent multiple buffers
        LEFT       = GL_LEFT,
        RIGHT      = GL_RIGHT,
        FRONT      = GL_FRONT,
        BACK       = GL_BACK,
        FRONT_BACK = GL_FRONT_AND_BACK
    };

    /**
     * The Framebuffer object is not actually a buffer, 
     * but an aggregator object that contains one or more attachments, 
     * which by their turn, are the actual buffers.
    */
    class FrameBuffer
    {
    public:
        FrameBuffer(GLsizei size)
        {
            _num = size;
            if(_num > 0)
                glGenFramebuffers(size, &_fbo);
        }

        FrameBuffer()
        {
            _num = 1;
            glGenFramebuffers(1, &_fbo);
        }

        ~FrameBuffer()
        {
            release();
        }

        FrameBuffer(const FrameBuffer &) = delete;
        FrameBuffer &operator=(const FrameBuffer &) = delete;

        FrameBuffer(FrameBuffer &&other)
        {
            _num = other._num;
            _fbo = other._fbo;
            other._num = 0;
        }

        FrameBuffer &operator=(FrameBuffer &&other)
        {
            release();
            _num = other._num;
            _fbo = other._fbo;
            other._num = 0;
            return *this;
        }

        void release()
        {
            if (_num > 0)
            {
                glDeleteBuffers(_num, &_fbo);
                _num = 0;
            }
        }

        void bind()
        {
            glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
        }

        void endBind()
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        GLsizei number()
        {
            return _num;
        }

        static inline FBStatus sCheckStatus()
        {
            return static_cast<FBStatus>(glCheckFramebufferStatus(GL_FRAMEBUFFER));
        }

        inline FBStatus checkStatus()
        {
            return static_cast<FBStatus>(glCheckNamedFramebufferStatus(_fbo, GL_FRAMEBUFFER));
        }

        static inline GLint sGetMaxColorAttachment()
        {
            GLint max;
            glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max);
            return max;
        }

        static inline void sReadBuffer(ColorBuffer buffer)
        {
            glReadBuffer(static_cast<GLenum>(buffer));
        }

        // only color attachments are accepted
        static inline void sReadBuffer(AttachmentType type)
        {
            glReadBuffer(static_cast<GLenum>(type));
        }

        static inline void sDrawBuffer(ColorBuffer buffer)
        {
            glDrawBuffer(static_cast<GLenum>(buffer));
        }
        // only color attachments are accepted
        static inline void sDrawBuffer(AttachmentType type)
        {
            glDrawBuffer(static_cast<GLenum>(type));
        }

        /**
         * # Design Hints
         * 
         * ## textarget value
         * For glFramebufferTexture1D, if texture is not zero, then textarget must be 
         *      GL_TEXTURE_1D. 
         * For glFramebufferTexture2D, if texture is not zero, textarget must be one of 
         *      GL_TEXTURE_2D, 
         *      GL_TEXTURE_RECTANGLE, 1
         *      GL_TEXTURE_CUBE_MAP_POSITIVE_X, // CUBE_MAP
         *      GL_TEXTURE_CUBE_MAP_POSITIVE_Y, // CUBE_MAP
         *      GL_TEXTURE_CUBE_MAP_POSITIVE_Z, // CUBE_MAP
         *      GL_TEXTURE_CUBE_MAP_NEGATIVE_X, // CUBE_MAP
         *      GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, // CUBE_MAP
         *      GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, // CUBE_MAP 
         *      GL_TEXTURE_2D_MULTISAMPLE. 1
         * For glFramebufferTexture3D, if texture is not zero, then textarget must be 
         *      GL_TEXTURE_3D.
         * For glFramebufferTexture and glNamedFramebufferTexture, 
         *  if texture is the name of a 
         *      GL_TEXTURE_3D, 
         *      GL_TEXTURE_CUBE_MAP_ARRAY, 
         *      GL_TEXTURE_CUBE_MAP, 
         *      GL_TEXTURE_2D_MULTISAMPLE
         *      GL_TEXTURE_2D_MULTISAMPLE_ARRAY
         *      GL_TEXTURE_1D_ARRAY
         *      GL_TEXTURE_2D_ARRAY
         *  the specified texture level is an array of images, and the framebuffer attachment is considered to be layered.
         * 
         * ## texture value
         * If texture is non-zero, the specified level of the texture object named texture 
         *  is attached to the framebuffer attachment point named by attachment. 
         * For glFramebufferTexture1D, glFramebufferTexture2D, and glFramebufferTexture3D, 
         *  texture must be 
         *  + zero
         *  + the name of an existing texture with an effective target of textarget
         *  + name of an existing cube-map texture(see cube map direction)
         * 
         * ## level value
         * If textarget is GL_TEXTURE_RECTANGLE, GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_2D_MULTISAMPLE_ARRAY 
         * then level must be zero.
         * 
         * If textarget is GL_TEXTURE_3D
         * then level must be 
         *  greater than or equal to zero and (>=0)
         *  less than or equal to log2 of the value of GL_MAX_3D_TEXTURE_SIZE. (<= log2(GL_MAX_3D_TEXTURE_SIZE))
         * 
         * If textarget is one of CubeDirection, then level must be 
         *  greater than or equal to zero and (>=0)
         *  less than or equal to log2 of the value of GL_MAX_CUBE_MAP_TEXTURE_SIZE. (<= log2(GL_MAX_CUBE_MAP_TEXTURE_SIZE))
         * 
         * For all other values of textarget, level must be 
         *  greater than or equal to zero and (>=0)
         *  less than or equal to log2 of the value of GL_MAX_TEXTURE_SIZE.
        */

        /**
         * @brief static functions for add color attachment
         * @param level If TextureType is TextureType::THREE_DIM, then level must 
         *  be greater than or equal to zero and less than or equal to log2 of the value of GL_MAX_3D_TEXTURE_SIZE.
        */
        template<TextureType ttype>
        static inline void sAddAttachment(TTextureBase<ttype>& texture, AttachmentType type, GLint level = 0)
        requires (ttype != TextureType::CUBE_MAP && !levelzerotex<ttype>)
        {
            auto _type = static_cast<GLenum>(type);
            glFramebufferTexture2D(GL_FRAMEBUFFER, _type, static_cast<GLenum>(ttype) ,texture.rawObject(), level);
        }
        template<TextureType ttype>
        static inline void sAddAttachment(TTextureBase<ttype>& texture, AttachmentType type)
        requires levelzerotex<ttype>
        {
            auto _type = static_cast<GLenum>(type);
            glFramebufferTexture2D(GL_FRAMEBUFFER, _type, static_cast<GLenum>(ttype) ,texture.rawObject(), 0);
        }
        // for cube texture
        static inline void sAddAttachment(CubeTexture& texture, CubeTextureDirection direction, AttachmentType type, GLint level)
        {
            auto _type = static_cast<GLenum>(type);
            glFramebufferTexture2D(GL_FRAMEBUFFER, _type, static_cast<GLenum>(direction), texture.rawObject(), level);
        }
        // for render buffer
        static inline void sAddAttachment(RenderBuffer& render_buffer, AttachmentType type)
        {
            auto _type = static_cast<GLenum>(type);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, _type, GL_RENDERBUFFER, render_buffer.rawObject());
        }

#ifdef __GLPP_SUPPORT_DSA
        void readBuffer(ColorBuffer buffer)
        {
            glNamedFramebufferReadBuffer(_fbo, static_cast<GLenum>(buffer));
        }
        // only color attachments are accepted
        void readBuffer(AttachmentType type)
        {
            glNamedFramebufferReadBuffer(_fbo, static_cast<GLenum>(type));
        }

        void drawBuffer(ColorBuffer buffer)
        {
            glNamedFramebufferDrawBuffer(_fbo, static_cast<GLenum>(buffer));
        }
        // only color attachments are accepted
        void drawBuffer(AttachmentType type)
        {
            glNamedFramebufferDrawBuffer(_fbo, static_cast<GLenum>(type));
        }

        /**
         * @brief functions for add color attachment, need opengl >= 4.5
         * @param level If TextureType is TextureType::THREE_DIM, then level must 
         *  be greater than or equal to zero and less than or equal to log2 of the value of GL_MAX_3D_TEXTURE_SIZE.
        */
        template<TextureType ttype>
        inline void addAttachment(TTextureBase<ttype>& texture, AttachmentType type, GLint level)
        requires (ttype != TextureType::CUBE_MAP && !levelzerotex<ttype>)
        {
            auto _type = static_cast<GLenum>(type);
            glNamedFramebufferTexture(_fbo, _type, texture.rawObject(), level);
        }
        /**
         * @brief functions for add color attachment, need opengl >= 4.5
         * @param level If TextureType is TextureType::THREE_DIM, then level must 
         *  be greater than or equal to zero and less than or equal to log2 of the value of GL_MAX_3D_TEXTURE_SIZE.
        */
        template<TextureType ttype>
        inline void addAttachment(TTextureBase<ttype>& texture, AttachmentType type)
        requires levelzerotex<ttype>
        {
            auto _type = static_cast<GLenum>(type);
            glNamedFramebufferTexture(_fbo, _type, texture.rawObject());
        }
        // for cube texture
        // inline void addAttachment(CubeTexture& texture, CubeTextureDirection direction, GLenum type, GLint level)
        // {
        //     auto _type = static_cast<GLenum>(type);
        //     glNamedFramebufferTexture(_fbo, _type, static_cast<GLenum>(direction), texture.rawObject(), level);
        // }
        // for render buffer
        inline void addAttachment(RenderBuffer& render_buffer, AttachmentType type)
        {
            auto _type = static_cast<GLenum>(type);
            glNamedFramebufferRenderbuffer(_fbo, _type, GL_RENDERBUFFER, render_buffer.rawObject());
        }
#endif

    private:
        GLsizei _num;
        GLuint  _fbo;
    };
}

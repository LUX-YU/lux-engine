#pragma once
#include <glad/glad.h>
#include <lux/opengl3/Texture.hpp>
#include <lux/opengl3/FrameBuffer.hpp>

namespace lux::gapiwrap::opengl
{
    enum class AttachmentType
    {
        COLOR_ATTACHMENT,
        DEPTH_ATTACHMENT,
        STENCIL_ATTACHMENT,
        DEPTH_STENCIL_ATTACHMENT
    };

    class Attachment
    {
    public:
        inline AttachmentType type() const
        {
            return _type;
        }

        inline GLenum value() const
        {
            return _value;
        }

    protected:
        Attachment(AttachmentType type, GLenum value)
        : _type(type), _value(value){}

    private:
        GLenum          _value;
        AttachmentType  _type;
    };

    class ColorAttachment : public Attachment
    {
    public:
        // index's range is [0, 31]
        ColorAttachment(GLuint index)
            : Attachment(AttachmentType::COLOR_ATTACHMENT, GL_COLOR_ATTACHMENT0 + index){}
    };

    class StencilAttachment : public Attachment
    {
    public:
        StencilAttachment()
            : Attachment(AttachmentType::STENCIL_ATTACHMENT, GL_STENCIL_ATTACHMENT){}
    };

    class DepthAttachment : public Attachment
    {
    public:
        DepthAttachment()
            : Attachment(AttachmentType::DEPTH_ATTACHMENT, GL_DEPTH_ATTACHMENT){}
    };
    
    class DepthStencilAttachment : public Attachment
    {
    public:
        DepthStencilAttachment()
        : Attachment(AttachmentType::DEPTH_STENCIL_ATTACHMENT, GL_DEPTH_STENCIL_ATTACHMENT){}
    };

    class FrameBuffer
    {
    public:
        FrameBuffer(GLsizei size)
        {
            _num = size;
            glGenFramebuffers(size, &_fbo);
        }

        ~FrameBuffer()
        {
            if (_num > 0)
                glDeleteFramebuffers(_num, &_fbo);
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

        GLsizei number()
        {
            return _num;
        }

        enum class EStatus
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

        static inline EStatus staticCheckStatus()
        {
            return static_cast<EStatus>(glCheckFramebufferStatus(GL_FRAMEBUFFER));
        }

        inline EStatus CheckStatus()
        {
            return static_cast<EStatus>(glCheckNamedFramebufferStatus(_fbo, GL_FRAMEBUFFER));
        }

        static inline void staticAddAttachment(Texture2D& texture, Attachment& attachment)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment.value(), GL_TEXTURE_2D, texture.rawObject(), 0);
        }

        static inline void staticAddAttachment(CubeMapTexture& texture, Attachment& attachment, CubeMapTexture::Direction& direction)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment.value(), static_cast<GLenum>(direction), texture.rawObject(), 0);
        }

        /*
            @brief Attach a renderbuffer as a logical buffer of a framebuffer object
        */
        static inline void staticAddAttachment(RenderBuffer& render_buffer, Attachment& attachment)
        {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment.value() ,GL_RENDERBUFFER, render_buffer.rawObject());
        }

    private:
        GLsizei _num;
        GLuint _fbo;
    };
}
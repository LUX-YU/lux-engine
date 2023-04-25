#pragma once
#include <glad/glad.h>
#include "config.h"
#include "ValueTypes.hpp"

namespace lux::gapi::opengl
{
    class RenderBuffer
    {
    public:
        explicit RenderBuffer(GLsizei num)
        {
            _num = num;
            if(_num > 0)
                glGenRenderbuffers(num, &_rbo);
        }

        RenderBuffer()
        {
            _num = 1;
            glGenRenderbuffers(1, &_rbo);
        }

        ~RenderBuffer()
        {
            release();
        }

        void release()
        {
            if(_num > 0)
            {
                glDeleteRenderbuffers(_num, &_rbo);
                _num = 0;
            }
        }

        [[nodiscard]] GLuint rawObject()
        {
            return _rbo;
        }

        void bind()
        {
            glBindRenderbuffer(GL_RENDERBUFFER, _rbo);
        }

        static void endBind()
        {
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }

        [[nodiscard]] GLsizei number() const
        {
            return _num;
        }

        RenderBuffer(const RenderBuffer&) = delete;
        RenderBuffer& operator=(const RenderBuffer&) = delete;

        RenderBuffer(RenderBuffer &&other)
        {
            _num = other._num;
            _rbo = other._rbo;
            other._num = 0;
        }

        RenderBuffer &operator=(RenderBuffer &&other) noexcept
        {
            release();

            _num = other._num;
            _rbo = other._rbo;
            other._num = 0;
            return *this;
        }

        /*
            @brief  Establish data storage, format and dimensions of a renderbuffer object's image
                    Equivalent to calling glRenderbufferStorageMultisample with the samples set to zero

            @param internalformat Specifies the internal format to use for the renderbuffer object's image.
            @param width Specifies the width of the renderbuffer, in pixels.
            @param height Specifies the height of the renderbuffer, in pixels.
        */
        static void staticStorage(ImageFormat internalformat, GLsizei width, GLsizei height)
        {
            // target
            // Specifies a binding target of the allocation for glRenderbufferStorage function. Must be GL_RENDERBUFFER.
            glRenderbufferStorage(GL_RENDERBUFFER, static_cast<GLenum>(internalformat), width, height);
        }
        /*
            @param samples          Specifies the number of samples to be used for the renderbuffer object's storage.
            @param internalformat   Specifies the internal format to use for the renderbuffer object's image.
            @param width            Specifies the width of the renderbuffer, in pixels.
            @param height           Specifies the height of the renderbuffer, in pixels.
        */
        static void staticStorageMultiSample(GLsizei samples, ImageFormat internalformat, GLsizei width, GLsizei height)
        {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, static_cast<GLenum>(internalformat), width, height);
        }

#ifdef __GLPP_SUPPORT_DSA
        /*
            @brief  Establish data storage, format and dimensions of a renderbuffer object's image
                    Equivalent to calling glRenderbufferStorageMultisample with the samples set to zero

            @param internalformat Specifies the internal format to use for the renderbuffer object's image.
            @param width Specifies the width of the renderbuffer, in pixels.
            @param height Specifies the height of the renderbuffer, in pixels.
        */
        void storage(ImageFormat internalformat, GLsizei width, GLsizei height)
        {
            // renderbuffe Specifies the name of the renderbuffer object for glNamedRenderbufferStorage function.
            glNamedRenderbufferStorage(_rbo, static_cast<GLenum>(internalformat), width, height);
        }

        void storageMultiSample(GLsizei samples, ImageFormat internalformat, GLsizei width, GLsizei height)
        {
            glNamedRenderbufferStorageMultisample(_rbo, samples, static_cast<GLenum>(internalformat), width, height);
        }
#endif

    private:
        GLsizei _num;
        GLuint  _rbo{};
    };
}

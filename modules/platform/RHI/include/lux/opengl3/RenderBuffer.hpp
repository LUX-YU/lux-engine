#pragma once
#include <glad/glad.h>

namespace lux::gapiwrap::opengl
{
    class RenderBuffer
    {
    public:
        RenderBuffer(GLsizei num)
        {
            _num = num;
            glGenRenderbuffers(num, &_rbo);
        }

        ~RenderBuffer()
        {
            if(_num > 0)
                glDeleteRenderbuffers(_num, &_rbo);
        }

        inline void release()
        {
            if(_num > 0)
            {
                glDeleteRenderbuffers(_num, &_rbo);
                _num = 0;
            }
        }

        inline GLuint rawObject()
        {
            return _rbo;
        }

        void bind()
        {
            glBindRenderbuffer(GL_RENDERBUFFER, _rbo);
        }

        static inline void unbind()
        {
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }

        inline GLsizei number() const 
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

        RenderBuffer &operator=(RenderBuffer &&other)
        {
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
        static inline void staticStorage(GLenum internalformat, GLsizei width, GLsizei height)
        {
            // target
            // Specifies a binding target of the allocation for glRenderbufferStorage function. Must be GL_RENDERBUFFER.
            glRenderbufferStorage(GL_RENDERBUFFER, internalformat, width, height);
        }
        
        /*
            @param samples          Specifies the number of samples to be used for the renderbuffer object's storage.
            @param internalformat   Specifies the internal format to use for the renderbuffer object's image.
            @param width            Specifies the width of the renderbuffer, in pixels.
            @param height           Specifies the height of the renderbuffer, in pixels.
        */
        static inline void staticStorageMultiSample(GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
        {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internalformat, width, height);
        }

        /*
            @brief  Establish data storage, format and dimensions of a renderbuffer object's image
                    Equivalent to calling glRenderbufferStorageMultisample with the samples set to zero

            @param internalformat Specifies the internal format to use for the renderbuffer object's image.
            @param width Specifies the width of the renderbuffer, in pixels.
            @param height Specifies the height of the renderbuffer, in pixels.
        */
        inline void storage(GLenum internalformat, GLsizei width, GLsizei height)
        {
            // renderbuffe Specifies the name of the renderbuffer object for glNamedRenderbufferStorage function.
            glNamedRenderbufferStorage(_rbo, internalformat, width, height);
        }

        inline void storageMultiSample(GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
        {
            glNamedRenderbufferStorageMultisample(_rbo, samples, internalformat, width, height);
        }

    private:
        GLsizei _num;
        GLuint  _rbo;
    };
}

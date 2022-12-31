#pragma once
#include <glad/glad.h>
#include <string>

namespace lux::gapiwrap::opengl
{
    class VertexBuffer
    {
    public:
        VertexBuffer(GLsizei number)
        :_num(number){
            glGenBuffers(number, &_vbo);
        }

        VertexBuffer(const VertexBuffer&) = delete;
        VertexBuffer& operator=(const VertexBuffer&) = delete;

        VertexBuffer(VertexBuffer&& other)
        {
            _num = other._num;
            _vbo = other._vbo;
            other._num = 0;
        }

        VertexBuffer& operator=(VertexBuffer&& other)
        {
            _num = other._num;
            _vbo = other._vbo;
            other._num = 0;
            return *this;
        }

        ~VertexBuffer()
        {
            if(_num > 0)
                glDeleteBuffers(_num, &_vbo);
        }

        void release()
        {
            if(_num > 0)
            {
                glDeleteBuffers(_num, &_vbo);
                _num = 0;
            }
        }

        inline GLuint rawObject()
        {
            return _vbo;
        }

        inline GLuint number() const
        {
            return _num;
        }

        inline void bind()
        {
            glBindBuffer(GL_ARRAY_BUFFER, _vbo);
        }

    private:
        GLsizei _num;
        GLuint  _vbo;
    };
}

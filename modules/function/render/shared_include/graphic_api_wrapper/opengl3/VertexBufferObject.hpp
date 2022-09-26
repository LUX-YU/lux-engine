#pragma once
#include <glad/glad.h>
#include <string>


namespace lux::engine::function
{
    class GlVertexBufferObject
    {
    public:
        GlVertexBufferObject(GLsizei number)
        :_number(number){
            glGenBuffers(number, &_vbo);
        }

        ~GlVertexBufferObject()
        {
            glDeleteBuffers(_number, &_vbo);
        }

        GLuint number()
        {
            return _number;
        }

        void bindBuffer(GLenum target)
        {
            glBindBuffer(target, _vbo);
        }

    private:
        GLsizei _number;
        GLuint  _vbo;
    };
}

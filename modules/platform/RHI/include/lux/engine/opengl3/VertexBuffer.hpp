#pragma once
#include <glad/glad.h>
#include <string>

namespace lux::gapi::opengl
{
    enum class BufferType
    {
        ARRAY_BUFFER                = GL_ARRAY_BUFFER, 	                // Vertex attributes
        ATOMIC_COUNTER_BUFFER       = GL_ATOMIC_COUNTER_BUFFER, 	    // Atomic counter storage
        COPY_READ_BUFFER            = GL_COPY_READ_BUFFER, 	            // Buffer copy source
        COPY_WRITE_BUFFER           = GL_COPY_WRITE_BUFFER, 	        // Buffer copy destination
        DISPATCH_INDIRECT_BUFFER    = GL_DISPATCH_INDIRECT_BUFFER, 	    // Indirect compute dispatch commands
        DRAW_INDIRECT_BUFFER        = GL_DRAW_INDIRECT_BUFFER, 	        // Indirect command arguments
        ELEMENT_ARRAY_BUFFER        = GL_ELEMENT_ARRAY_BUFFER, 	        // Vertex array indices
        PIXEL_PACK_BUFFER           = GL_PIXEL_PACK_BUFFER, 	        // Pixel read target
        PIXEL_UNPACK_BUFFER         = GL_PIXEL_UNPACK_BUFFER, 	        // Texture data source
        QUERY_BUFFER                = GL_QUERY_BUFFER, 	                // Query result buffer
        SHADER_STORAGE_BUFFER       = GL_SHADER_STORAGE_BUFFER, 	    // Read-write storage for shaders
        TEXTURE_BUFFER              = GL_TEXTURE_BUFFER, 	            // Texture data buffer
        TRANSFORM_FEEDBACK_BUFFER   = GL_TRANSFORM_FEEDBACK_BUFFER, 	// Transform feedback buffer
        UNIFORM_BUFFER              = GL_UNIFORM_BUFFER,                // Uniform block storage
    };

    enum class BufferDataUsage
    {
        STREAM_DRAW  = GL_STREAM_DRAW,
        STREAM_READ  = GL_STREAM_READ,
        STREAM_COPY  = GL_STREAM_COPY,
        STATIC_DRAW  = GL_STATIC_DRAW,
        STATIC_READ  = GL_STATIC_READ,
        STATIC_COPY  = GL_STATIC_COPY,
        DYNAMIC_DRAW = GL_DYNAMIC_DRAW,
        DYNAMIC_READ = GL_DYNAMIC_READ,
        DYNAMIC_COPY = GL_DYNAMIC_COPY
    };

    // vertex buffer object
    class VertexBufferBase
    {
    public:
        ~VertexBufferBase()
        {
            release();
        }

        VertexBufferBase(const VertexBufferBase&) = delete;
        VertexBufferBase& operator=(const VertexBufferBase&) = delete;

        VertexBufferBase(VertexBufferBase&& other)
        {
            _num = other._num;
            _vbo = other._vbo;
            other._num = 0;
        }

        VertexBufferBase& operator=(VertexBufferBase&& other)
        {
            release();
            _num = other._num;
            _vbo = other._vbo;
            other._num = 0;
            return *this;
        }

        void release()
        {
            if(_num > 0)
            {
                glDeleteBuffers(_num, &_vbo);
                _num = 0;
            }
        }

        GLuint rawObject()
        {
            return _vbo;
        }

        [[nodiscard]] GLuint number() const
        {
            return _num;
        }

    protected:

        explicit VertexBufferBase(GLsizei number)
        :_num(number){
            if(_num > 0)
            glGenBuffers(_num, &_vbo);
        }

        GLsizei _num;
        GLuint  _vbo{};
    };

    template<BufferType btype>
    class TVertexBuffer : VertexBufferBase
    {
    public:
        static constexpr auto buffer_type = static_cast<GLenum>(btype);

        explicit TVertexBuffer(GLsizei size) : VertexBufferBase(size){}
        TVertexBuffer() : VertexBufferBase(1){}

        void bind()
        {
            glBindBuffer(buffer_type, _vbo);
        }

        void endBind()
        {
            glBindBuffer(buffer_type, 0);
        }

        static void sBufferData(GLsizeiptr size, const void* data, BufferDataUsage usage)
        {
            glBufferData(buffer_type, size, data, static_cast<GLenum>(usage));
        }

        static void sBufferSubData(GLintptr offset, GLsizeiptr size, const void * data)
        {
            glBufferSubData(buffer_type, offset, size, data);
        }
#ifdef __GLPP_SUPPORT_DSA
        // need opengl >= 4.5
        void bufferData(GLsizeiptr size, const void* data, BufferDataUsage usage)
        {
            glNamedBufferData(_vbo, size, data, static_cast<GLenum>(usage));
        }

        // need opengl >= 4.5
        void bufferSubData(GLintptr offset, GLsizeiptr size, const void * data)
        {
            glNamedBufferSubData(_vbo, offset, size, data);
        }
#endif
    };

    using ArrayBuffer           = TVertexBuffer<BufferType::ARRAY_BUFFER>;
    using ElementArrayBuffer    = TVertexBuffer<BufferType::ELEMENT_ARRAY_BUFFER>;
}

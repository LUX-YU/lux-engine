#include "lux/engine/RHI/gl4RHI/GLRHI.hpp"
#include "lux/engine/RHI/gl4RHI/GLResources.hpp"

namespace lux::engine::rhi::gl
{
	static ::lux::gapi::opengl::EBufferType toGLBufferType(EBufferUsage usage)
	{
		using namespace ::lux::gapi::opengl;
		EBufferType ret_type{EBufferType::ARRAY_BUFFER};
		switch (usage)
		{
		case EBufferUsage::VERTEX:
		{
			ret_type = ::lux::gapi::opengl::EBufferType::ARRAY_BUFFER;
			break;
		}
		case EBufferUsage::INDEX:
		{
			ret_type = ::lux::gapi::opengl::EBufferType::ELEMENT_ARRAY_BUFFER;
			break;
		}
		};

		return ret_type;
	}

	OpenGLBuffer::OpenGLBuffer(const BufferCreateInfo& info)
	: Buffer(info), _buffer(1, toGLBufferType(info.info.usage))
	{

	}

	std::unique_ptr<Buffer> OpenGLRHI::createBuffer(const BufferCreateInfo& info)
	{
		return std::make_unique<OpenGLBuffer>(info);
	}
}
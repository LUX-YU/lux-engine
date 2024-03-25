#pragma once
#include "lux/engine/RHI/Resources.hpp"
#include "lux/engine/RHI/Viewport.hpp"

#define __GLPP_SUPPORT_DSA
#include "lux/engine/opengl3/VertexBuffer.hpp"

namespace lux::engine::rhi::gl
{
	class OpenGLBuffer : public ::lux::engine::rhi::Buffer
	{
	public:
		explicit OpenGLBuffer(const BufferCreateInfo& info);

	private:
		::lux::gapi::opengl::Buffer _buffer;
	};

	class OpenGLViewport : public ::lux::engine::rhi::Viewport
	{

	};
}

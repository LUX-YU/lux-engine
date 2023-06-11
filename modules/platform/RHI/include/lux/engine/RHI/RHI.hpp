#pragma once
#include <string>
#include "ShaderDefine.hpp"

namespace lux::rhi
{
	class RHIVertexBufferRef;

	class RenderResource
	{
	public:

	};

	class RHI
	{
	public:
		virtual ~RHI() = 0;

		virtual bool init() = 0;

		// like opengl ,directx12 or vulkan
		virtual std::string name() = 0;

		struct  VertexBufferCreateInfo
		{
			
		};

		virtual RHIVertexBufferRef createVertexBuffer() = 0;
	};
}

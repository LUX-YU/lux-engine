#pragma once
#include <string>
#include <memory>
#include "Resources.hpp"

namespace lux::engine::rhi
{
	class Buffer;
	class Viewport;

	enum class EGraphAPITypes
	{
		OpenGL4,
		Vulkan,
		DirectX12
	};

	class RHI
	{
	public:
		virtual ~RHI(){}

		virtual std::unique_ptr<Viewport> createViewport(uint32_t width, uint32_t height) = 0;

		virtual std::unique_ptr<Buffer> createBuffer(const BufferCreateInfo&) = 0;

		virtual EGraphAPITypes type() const = 0;
	};

	class RHICreator
	{
	public:
		std::unique_ptr<RHI> create(EGraphAPITypes);
	};
}

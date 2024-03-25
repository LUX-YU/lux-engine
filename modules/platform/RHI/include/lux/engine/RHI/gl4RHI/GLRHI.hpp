#pragma once
#include "lux/engine/RHI/RHI.hpp"

namespace lux::engine::rhi::gl
{
	class OpenGLRHI : public ::lux::engine::rhi::RHI
	{
	public:
		OpenGLRHI();

		std::unique_ptr<Buffer> createBuffer(const BufferCreateInfo&) override;

		EGraphAPITypes type() const final override;
	};
}

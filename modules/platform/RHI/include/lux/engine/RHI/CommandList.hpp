#pragma once

namespace lux::engine::rhi
{
	class CommandList
	{
	public:
		void beginRenderPass();
		void endRenderPass();
	};

}
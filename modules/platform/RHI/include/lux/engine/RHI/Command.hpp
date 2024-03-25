#pragma once

namespace lux::engine::rhi
{
	class CommandBase
	{
	public:
		virtual void execute() = 0;
	};
}
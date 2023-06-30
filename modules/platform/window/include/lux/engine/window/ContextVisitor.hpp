#pragma once
#include <memory>

namespace lux::window
{
	class GLContext;
	class VulkanContext;

	class ContextVisitor
	{
	public:
		virtual bool visitContext(GLContext*) = 0;
		virtual bool visitContext(VulkanContext*) = 0;
	};
}
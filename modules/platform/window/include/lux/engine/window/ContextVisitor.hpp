#pragma once
#include <memory>

namespace lux::window
{
	class GLContext;
	class VulkanContext;

	class ContextVisitor
	{
	public:
		virtual bool visitContext(GLContext*, int operation) = 0;
		virtual bool visitContext(VulkanContext*, int operation) = 0;
	};
}

#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include <vulkan/vulkan.h>

#include <vector>

namespace lux::gapi::vk
{
	class LogicalDevice;

	class Surface
	{
	public:
		/* Forget this, We only accept a created surface so far
		 * just like using glfwCreateWindowsSurface to create a surface
		Surface(VkDevice device)
		{

		}
		*/
		Surface() : surface{ VK_NULL_HANDLE } {}

		Surface(VkSurfaceKHR surface)
			: surface{ surface } {}

		Surface(const Surface&) = delete;
		Surface& operator=(const Surface&) = delete;

		Surface(Surface&& other) noexcept
		{
			surface = other.surface;
			other.surface = VK_NULL_HANDLE;
		}

		Surface& operator=(Surface&& other) noexcept
		{
			surface = other.surface;
			other.surface = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkInstance instance, VkAllocationCallbacks* allocator = nullptr)
		{
			if (surface != VK_NULL_HANDLE)
			{
				vkDestroySurfaceKHR(instance, surface, allocator);
				surface = VK_NULL_HANDLE;
			}
		}

		operator VkSurfaceKHR() const noexcept { return surface; }
		const VkSurfaceKHR* operator&() const noexcept { return &surface; }

		inline VkSurfaceKHR handle() const noexcept { return surface; }
		inline const VkSurfaceKHR* handlePtr() const noexcept { return &surface; }

	private:
		VkSurfaceKHR surface{ VK_NULL_HANDLE };
	};
}

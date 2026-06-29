#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
	class Fence
	{
	public:
		Fence() : fence(VK_NULL_HANDLE) {}

		Fence(VkDevice device, const VkFenceCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
		{
			VK_FUNC_INVOKE(vkCreateFence, "Failed to create Fence object", device, &info, allocator, &fence);
		}

		Fence(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			VkFenceCreateInfo info;
			info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			info.pNext = nullptr;
			info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VK_FUNC_INVOKE(vkCreateFence, "Failed to create Fence object", device, &info, allocator, &fence);
		}

		Fence(const Fence&) = delete;
		Fence& operator=(const Fence&) = delete;

		Fence(Fence&& other) noexcept
		{
			fence = other.fence;
			other.fence = VK_NULL_HANDLE;
		}

		Fence& operator=(Fence&& other) noexcept
		{
			fence = other.fence;
			other.fence = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			if (fence != VK_NULL_HANDLE)
			{
				vkDestroyFence(device, fence, allocator);
				fence = VK_NULL_HANDLE;
			}
		}

		void wait(VkDevice device)
		{
			VK_FUNC_INVOKE(vkWaitForFences, "Failed to wait for Fence", device, 1, &fence, VK_TRUE, UINT64_MAX);
		}

		void wait(VkDevice device, uint64_t timeout)
		{
			VK_FUNC_INVOKE(vkWaitForFences, "Failed to wait for Fence", device, 1, &fence, VK_TRUE, timeout);
		}

		void reset(VkDevice device)
		{
			VK_FUNC_INVOKE(vkResetFences, "Failed to reset Fence", device, 1, &fence);
		}

		inline operator VkFence() const noexcept { return fence; }
		inline const VkFence* operator&() const noexcept { return &fence; }

		inline VkFence handle() const noexcept { return fence; }
		inline const VkFence* handlePtr() const noexcept { return &fence; }

	private:
		VkFence fence;
	};
}
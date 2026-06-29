#pragma once
#include "lux/engine/gapi/vk/Object.hpp"

namespace lux::gapi::vk
{
	class CommandPoolBuilder;
	class CommandPool
	{
	public:
		using Builder = CommandPoolBuilder;

		CommandPool	() : command_pool(VK_NULL_HANDLE) {}

		CommandPool(VkDevice device, const VkCommandPoolCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
		{
			VK_FUNC_INVOKE(vkCreateCommandPool, "Failed to create CommandPool object", device, &info, allocator, &command_pool);
		}

		CommandPool(VkDevice device, uint32_t queue_family_index, VkAllocationCallbacks* allocator = nullptr)
		{
			VkCommandPoolCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
				.queueFamilyIndex = queue_family_index
			};

			VK_FUNC_INVOKE(vkCreateCommandPool, "Failed to create CommandPool object", device, &info, allocator, &command_pool);
		}

		CommandPool(const CommandPool&) = delete;
		CommandPool& operator=(const CommandPool&) = delete;

		CommandPool(CommandPool&& other) noexcept
		{
			command_pool = other.command_pool;
			other.command_pool = VK_NULL_HANDLE;
		}

		CommandPool& operator=(CommandPool&& other) noexcept
		{
			command_pool = other.command_pool;
			other.command_pool = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			if (command_pool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(device, command_pool, allocator);
				command_pool = VK_NULL_HANDLE;
			}
		}

		inline operator VkCommandPool() const noexcept { return command_pool; }
		inline const VkCommandPool* operator&() const noexcept { return &command_pool; }

		inline VkCommandPool handle() const noexcept { return command_pool; }
		inline const VkCommandPool* handlePtr() const noexcept { return &command_pool; }
	
	private:
		VkCommandPool command_pool{ VK_NULL_HANDLE };
	};

	class CommandPoolBuilder
	{
	public:
		CommandPoolBuilder()
		{
			// Set default values
			info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			info.pNext = nullptr;
			info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		}

		CommandPoolBuilder& setQueueFamilyIndex(uint32_t queue_family_index)
		{
			info.queueFamilyIndex = queue_family_index;
			return *this;
		}

		CommandPool build(VkDevice device, VkAllocationCallbacks* allocator = nullptr) const
		{
			return CommandPool(device, info, allocator);
		}

	private:
		VkCommandPoolCreateInfo info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	};
}

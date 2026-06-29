#pragma once
#include "Object.hpp"

namespace lux::gapi::vk
{
	// copyable
	class Queue
	{
	public:
		Queue() : queue(VK_NULL_HANDLE) {}

		Queue(VkDevice device, uint32_t queue_family_index, uint32_t queue_index)
		{
			vkGetDeviceQueue(device, queue_family_index, queue_index, &queue);
		}

		operator VkQueue() const noexcept { return queue; }
		const VkQueue* operator&() const noexcept { return &queue; }

		inline VkQueue handle() const noexcept { return queue; }
		inline const VkQueue* handlePtr() const noexcept { return &queue; }

	private:
		VkQueue						   queue{ VK_NULL_HANDLE };
	};
}


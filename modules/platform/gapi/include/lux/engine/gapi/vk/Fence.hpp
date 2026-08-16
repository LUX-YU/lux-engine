#pragma once
#include <vulkan/vulkan.h>

#include <utility>

namespace lux::gapi::vk
{
	class Fence
	{
	public:
		Fence() noexcept = default;

		/// Adopt an already-created handle. Creation is deliberately kept out of
		/// this carrier: Vulkan failures must be returned by the owning factory,
		/// rather than hidden inside a constructor that can only print and continue.
		[[nodiscard]] static Fence adopt(VkFence handle) noexcept
		{
			Fence result;
			result.fence = handle;
			return result;
		}

		Fence(const Fence&) = delete;
		Fence& operator=(const Fence&) = delete;

		Fence(Fence&& other) noexcept
			: fence(std::exchange(other.fence, VkFence{}))
		{
		}

		// Rebinding a live Vulkan owner cannot be correct without the device and
		// allocator needed to release its current handle. No caller needs assignment;
		// keep the ownership transition construction-only instead of providing an
		// operation that silently leaks the overwritten fence.
		Fence& operator=(Fence&& other) noexcept = delete;

		void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			if (fence != VK_NULL_HANDLE)
			{
				vkDestroyFence(device, fence, allocator);
				fence = VK_NULL_HANDLE;
			}
		}

		/// Wait for this fence, RETURNING the result.
		///
		/// The result is not optional to look at, which is why these no longer
		/// go through VK_FUNC_INVOKE: that macro prints and then continues, and
		/// "continue" is the one thing a failed fence wait must not do. A fence
		/// is the proof that the GPU is finished with a frame slot's resources;
		/// proceeding without that proof means recycling command buffers,
		/// descriptors and images while the GPU may still be reading them.
		///
		/// Note that VK_TIMEOUT is reachable even here, where the timeout is
		/// UINT64_MAX and the spec calls the wait infinite: drivers impose
		/// their own cap (Adreno 830 returns VK_TIMEOUT after ~3 s). So an
		/// "infinite" wait is still a call whose result must be checked.
		[[nodiscard]] VkResult wait(VkDevice device)
		{
			return vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
		}

		[[nodiscard]] VkResult wait(VkDevice device, uint64_t timeout)
		{
			return vkWaitForFences(device, 1, &fence, VK_TRUE, timeout);
		}

		[[nodiscard]] VkResult reset(VkDevice device) noexcept
		{
			return vkResetFences(device, 1, &fence);
		}

		inline operator VkFence() const noexcept { return fence; }
		inline const VkFence* operator&() const noexcept { return &fence; }

		inline VkFence handle() const noexcept { return fence; }
		inline const VkFence* handlePtr() const noexcept { return &fence; }

	private:
		VkFence fence{VK_NULL_HANDLE};
	};
}

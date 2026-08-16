#pragma once
#include "lux/engine/gapi/vk/ExternalHandle.hpp"
#include <vulkan/vulkan.h>

#include <utility>

namespace lux::gapi::vk
{
	class Semaphore
	{
	public:
		Semaphore() noexcept = default;
		~Semaphore() noexcept
		{
			reset();
		}

		/// Adopt an already-created handle together with everything required to
		/// destroy it. This is an owning operation; release() is the only way to
		/// detach the handle without destroying it.
		[[nodiscard]] static Semaphore adopt(
			VkDevice device,
			VkSemaphore handle,
			const VkAllocationCallbacks* allocator = nullptr
		) noexcept
		{
			Semaphore result;
			result.device_ = device;
			result.allocator_ = allocator;
			result.semaphore_ = handle;
			return result;
		}

		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;

		Semaphore(Semaphore&& other) noexcept
			: device_(std::exchange(other.device_, VkDevice{})),
			  allocator_(std::exchange(other.allocator_, nullptr)),
			  semaphore_(std::exchange(other.semaphore_, VkSemaphore{}))
		{
		}

		Semaphore& operator=(Semaphore&& other) noexcept
		{
			if (this == &other)
				return *this;

			reset();
			device_ = std::exchange(other.device_, VkDevice{});
			allocator_ = std::exchange(other.allocator_, nullptr);
			semaphore_ = std::exchange(other.semaphore_, VkSemaphore{});
			return *this;
		}

		/// Destroy the owned handle, if any, while retaining an empty state.
		void reset() noexcept
		{
			if (semaphore_ != VK_NULL_HANDLE)
				vkDestroySemaphore(device_, semaphore_, allocator_);
			device_ = VK_NULL_HANDLE;
			allocator_ = nullptr;
			semaphore_ = VK_NULL_HANDLE;
		}

		/// Detach ownership for publication into a raw-handle API.
		[[nodiscard]] VkSemaphore release() noexcept
		{
			device_ = VK_NULL_HANDLE;
			allocator_ = nullptr;
			return std::exchange(semaphore_, VkSemaphore{});
		}

		// Export this semaphore as a platform-neutral external handle (Win32 NT HANDLE or
		// POSIX fd; see ExternalHandle.hpp) for import by an external API
		// (e.g. CUDA cudaImportExternalSemaphore). Requires the semaphore to have been
		// created with VkExportSemaphoreCreateInfo (and, for cross-API timeline sync, a
		// VK_SEMAPHORE_TYPE_TIMELINE) and the matching external-semaphore extension
		// enabled (external_semaphore_win32 on Windows / external_semaphore_fd on POSIX).
		// Handle owned by the CALLER. Returns kInvalidExternalHandle on failure.
		[[nodiscard]] VkResult exportHandle(ExternalHandle& out_handle) const noexcept
		{
			out_handle = kInvalidExternalHandle;
			if (device_ == VK_NULL_HANDLE || semaphore_ == VK_NULL_HANDLE)
				return VK_ERROR_INITIALIZATION_FAILED;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
			auto fn = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
				vkGetDeviceProcAddr(device_, "vkGetSemaphoreWin32HandleKHR"));
			if (!fn)
				return VK_ERROR_EXTENSION_NOT_PRESENT;
			VkSemaphoreGetWin32HandleInfoKHR info{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
			info.semaphore  = semaphore_;
			info.handleType = kOpaqueExternalSemaphoreType;
			HANDLE handle = nullptr;
			const VkResult result = fn(device_, &info, &handle);
			if (result == VK_SUCCESS)
				out_handle = handle;
			return result;
#else
			auto fn = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
				vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
			if (!fn)
				return VK_ERROR_EXTENSION_NOT_PRESENT;
			VkSemaphoreGetFdInfoKHR info{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
			info.semaphore  = semaphore_;
			info.handleType = kOpaqueExternalSemaphoreType;
			int fd = -1;
			const VkResult result = fn(device_, &info, &fd);
			if (result == VK_SUCCESS)
				out_handle = fd;
			return result;
#endif
		}

		inline operator VkSemaphore() const noexcept { return semaphore_; }
		
		inline VkSemaphore handle() const noexcept { return semaphore_; }

	private:
		VkDevice                     device_{VK_NULL_HANDLE};
		const VkAllocationCallbacks* allocator_{nullptr};
		VkSemaphore                  semaphore_{VK_NULL_HANDLE};
	};
}

#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include "lux/engine/gapi/vk/ExternalHandle.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
	class SemaphoreBuilder;
	class Semaphore
	{
	public:
		using Builder = SemaphoreBuilder;

		Semaphore() : semaphore(VK_NULL_HANDLE) {}

		Semaphore(VkDevice device, const VkSemaphoreCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
		{
			VK_FUNC_INVOKE(vkCreateSemaphore, "Failed to create Semaphore object", device, &info, allocator, &semaphore);
		}

		Semaphore(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			VkSemaphoreCreateInfo info;
			info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			info.pNext = nullptr;
			// VkSemaphoreCreateFlags is a bitmask type for setting a mask, but is currently reserved for future use.
			info.flags = 0;

			VK_FUNC_INVOKE(vkCreateSemaphore, "Failed to create Semaphore object", device, &info, allocator, &semaphore);
		}

		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;

		Semaphore(Semaphore&& other) noexcept
		{
			semaphore = other.semaphore;
			other.semaphore = VK_NULL_HANDLE;
		}

		Semaphore& operator=(Semaphore&& other) noexcept
		{
			semaphore = other.semaphore;
			other.semaphore = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			if (semaphore != VK_NULL_HANDLE)
			{
				vkDestroySemaphore(device, semaphore, allocator);
				semaphore = VK_NULL_HANDLE;
			}
		}

		// Export this semaphore as a platform-neutral external handle (Win32 NT HANDLE or
		// POSIX fd; see ExternalHandle.hpp) for import by an external API
		// (e.g. CUDA cudaImportExternalSemaphore). Requires the semaphore to have been
		// created with VkExportSemaphoreCreateInfo (and, for cross-API timeline sync, a
		// VK_SEMAPHORE_TYPE_TIMELINE) and the matching external-semaphore extension
		// enabled (external_semaphore_win32 on Windows / external_semaphore_fd on POSIX).
		// Handle owned by the CALLER. Returns kInvalidExternalHandle on failure.
		ExternalHandle exportHandle(VkDevice device) const
		{
#if defined(VK_USE_PLATFORM_WIN32_KHR)
			auto fn = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
				vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR"));
			if (!fn)
				return kInvalidExternalHandle;
			VkSemaphoreGetWin32HandleInfoKHR info{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
			info.semaphore  = semaphore;
			info.handleType = kOpaqueExternalSemaphoreType;
			HANDLE handle = nullptr;
			if (fn(device, &info, &handle) != VK_SUCCESS)
				return kInvalidExternalHandle;
			return handle;
#else
			auto fn = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
				vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
			if (!fn)
				return kInvalidExternalHandle;
			VkSemaphoreGetFdInfoKHR info{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
			info.semaphore  = semaphore;
			info.handleType = kOpaqueExternalSemaphoreType;
			int fd = -1;
			if (fn(device, &info, &fd) != VK_SUCCESS)
				return kInvalidExternalHandle;
			return fd;
#endif
		}

		inline operator VkSemaphore() const noexcept { return semaphore; }
		inline const VkSemaphore* operator&() const noexcept { return &semaphore; }
		
		inline VkSemaphore handle() const noexcept { return semaphore; }
		inline const VkSemaphore* handlePtr() const noexcept { return &semaphore; }

	private:
		VkSemaphore semaphore{ VK_NULL_HANDLE };
	};
}
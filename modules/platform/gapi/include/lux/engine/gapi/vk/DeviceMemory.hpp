#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include "lux/engine/gapi/vk/ExternalHandle.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    class DeviceMemory
    {
    public:
        DeviceMemory() : memory_(VK_NULL_HANDLE)
        {
        }

        DeviceMemory(VkDevice dev, const VkMemoryAllocateInfo& ai, VkAllocationCallbacks* allocator = nullptr)
        {
            VK_FUNC_INVOKE(vkAllocateMemory, "Failed to allocate memory", dev, &ai, allocator, &memory_);
        }

        DeviceMemory(
            VkDevice dev,
            VkDeviceSize size,
            uint32_t memoryTypeIndex,
            VkAllocationCallbacks* allocator = nullptr
        )
        {
            VkMemoryAllocateInfo ai = {};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = size;
            ai.memoryTypeIndex = memoryTypeIndex;
            VK_FUNC_INVOKE(vkAllocateMemory, "Failed to allocate memory", dev, &ai, allocator, &memory_);
        }

        DeviceMemory(const DeviceMemory&) = delete;
        DeviceMemory& operator=(const DeviceMemory&) = delete;

        DeviceMemory(DeviceMemory&& other) noexcept
        {
            memory_ = other.memory_;
            other.memory_ = VK_NULL_HANDLE;
        }

        DeviceMemory& operator=(DeviceMemory&& other) noexcept
        {
            memory_ = other.memory_;
            other.memory_ = VK_NULL_HANDLE;
            return *this;
        }

        void release(VkDevice dev, VkAllocationCallbacks* allocator = nullptr)
        {
            if (memory_ != VK_NULL_HANDLE)
            {
                vkFreeMemory(dev, memory_, allocator);
                memory_ = VK_NULL_HANDLE;
            }
        }

        void* map(VkDevice device, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags = 0)
        {
            void* ptr = nullptr;
            VK_FUNC_INVOKE(vkMapMemory, "Failed to map device memory_", device, memory_, offset, size, flags, &ptr);
            return ptr;
        }

        void unmap(VkDevice device)
        {
            if (memory_ != VK_NULL_HANDLE)
                vkUnmapMemory(device, memory_);
        }

        // Export this memory as a platform-neutral external handle (Win32 NT HANDLE or
        // POSIX fd; see ExternalHandle.hpp) for import by an external API
        // (e.g. CUDA cudaImportExternalMemory). Requires the allocation to have been
        // made with VkExportMemoryAllocateInfo (+ usually a dedicated allocation that
        // bypasses VMA) and the matching external-memory extension enabled on the device
        // (external_memory_win32 on Windows / external_memory_fd on POSIX). The returned
        // handle is owned by the CALLER (CloseHandle / ::close once the importer has
        // dup'd it). Returns kInvalidExternalHandle on failure. The export fn is loaded
        // per-call via vkGetDeviceProcAddr (the engine links no volk dispatch table).
        ExternalHandle exportHandle(VkDevice dev) const
        {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            auto fn =
                reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(dev, "vkGetMemoryWin32HandleKHR"));
            if (!fn)
                return kInvalidExternalHandle;
            VkMemoryGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
            info.memory = memory_;
            info.handleType = kOpaqueExternalMemoryType;
            HANDLE handle = nullptr;
            if (fn(dev, &info, &handle) != VK_SUCCESS)
                return kInvalidExternalHandle;
            return handle;
#else
            auto fn = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR"));
            if (!fn)
                return kInvalidExternalHandle;
            VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
            info.memory = memory_;
            info.handleType = kOpaqueExternalMemoryType;
            int fd = -1;
            if (fn(dev, &info, &fd) != VK_SUCCESS)
                return kInvalidExternalHandle;
            return fd;
#endif
        }

        inline operator VkDeviceMemory() const noexcept
        {
            return memory_;
        }
        inline const VkDeviceMemory* operator&() const noexcept
        {
            return &memory_;
        }

        inline VkDeviceMemory handle() const noexcept
        {
            return memory_;
        }
        inline const VkDeviceMemory* handlePtr() const noexcept
        {
            return &memory_;
        }

    private:
        VkDeviceMemory memory_;
    };
}

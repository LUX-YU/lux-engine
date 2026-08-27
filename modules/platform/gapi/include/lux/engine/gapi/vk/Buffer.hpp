#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include "lux/engine/gapi/vk/DeviceMemory.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    class BufferBuilder;
    class Buffer
    {
    public:
        Buffer() : buffer(VK_NULL_HANDLE)
        {
        }

        Buffer(VkDevice device, const VkBufferCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
        {
            VK_FUNC_INVOKE(vkCreateBuffer, "Failed to create Buffer object", device, &info, allocator, &buffer);
        }

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        Buffer(Buffer&& other) noexcept
        {
            buffer = other.buffer;
            other.buffer = VK_NULL_HANDLE;
        }

        Buffer& operator=(Buffer&& other) noexcept
        {
            buffer = other.buffer;
            other.buffer = VK_NULL_HANDLE;
            return *this;
        }

        DeviceMemory allocateMemory(
            VkDevice device,
            VkDeviceSize allocation_size,
            uint32_t memory_type_index,
            VkAllocationCallbacks* allocator = nullptr
        )
        {
            return DeviceMemory{device, allocation_size, memory_type_index, allocator};
        }

        VkMemoryRequirements memoryRequirements(VkDevice dev)
        {
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(dev, buffer, &req);
            return req;
        }

        void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
        {
            if (buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(device, buffer, allocator);
                buffer = VK_NULL_HANDLE;
            }
        }

        void bindMemory(VkDevice logical_dev, VkDeviceMemory memory, VkDeviceSize offset = 0)
        {
            VK_FUNC_INVOKE(vkBindBufferMemory, "Failed to bind memory to buffer", logical_dev, buffer, memory, offset);
        }

        inline operator bool() const noexcept
        {
            return buffer != VK_NULL_HANDLE;
        }
        inline bool operator!() const noexcept
        {
            return buffer == VK_NULL_HANDLE;
        }

        inline operator VkBuffer() const noexcept
        {
            return buffer;
        }
        inline const VkBuffer* operator&() const noexcept
        {
            return &buffer;
        }

        inline VkBuffer handle() const noexcept
        {
            return buffer;
        }
        inline const VkBuffer* handlePtr() const noexcept
        {
            return &buffer;
        }

    private:
        VkBuffer buffer;
    };

    enum class EBufferFlag
    {
        SPARSE_BINDING = VK_BUFFER_CREATE_SPARSE_BINDING_BIT,
        SPARSE_RESIDENCY = VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        SPARSE_ALIASED = VK_BUFFER_CREATE_SPARSE_ALIASED_BIT,
        PROTECTED = VK_BUFFER_CREATE_PROTECTED_BIT,
        DEVICE_ADDRESS_CAPTURE_REPLAY = VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT,

#ifdef VK_BUFFER_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT
        DESCRIPTOR_BUFFER_CAPTURE_REPLAY_EXT = VK_BUFFER_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT,
#endif

#ifdef VK_BUFFER_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR
        VIDEO_PROFILE_INDEPENDENT_KHR = VK_BUFFER_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR,
#endif

#ifdef VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_EXT
        DEVICE_ADDRESS_CAPTURE_REPLAY_EXT = VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_EXT,
#endif

#ifdef VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_KHR
        DEVICE_ADDRESS_CAPTURE_REPLAY_KHR = VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_KHR,
#endif
    };

    enum class EBufferUsage
    {
        TRANSFER_SRC = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        TRANSFER_DST = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        UNIFORM_TEXEL_BUFFER = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
        STORAGE_TEXEL_BUFFER = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
        UNIFORM_BUFFER = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        STORAGE_BUFFER = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        INDEX_BUFFER = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VERTEX_BUFFER = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        INDIRECT_BUFFER = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        SHADER_DEVICE_ADDRESS_BIT = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,

#ifdef VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR
        VIDEO_DECODE_SRC_BIT_KHR = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR,
#endif
#ifdef VK_BUFFER_USAGE_VIDEO_DECODE_DST_BIT_KHR
        VIDEO_DECODE_DST_BIT_KHR = VK_BUFFER_USAGE_VIDEO_DECODE_DST_BIT_KHR,
#endif

#ifdef VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT
        TRANSFORM_FEEDBACK_BUFFER_BIT_EXT = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT,
#endif
#ifdef VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT
        TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT,
#endif
#ifdef VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT
        CONDITIONAL_RENDERING_BIT_EXT = VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
#endif

#ifdef VK_BUFFER_USAGE_EXECUTION_GRAPH_SCRATCH_BIT_AMDX
        EXECUTION_GRAPH_SCRATCH_BIT_AMDX = VK_BUFFER_USAGE_EXECUTION_GRAPH_SCRATCH_BIT_AMDX,
#endif

#ifdef VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
        ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
#endif
#ifdef VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
        ACCELERATION_STRUCTURE_STORAGE_BIT_KHR = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
#endif
#ifdef VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
        SHADER_BINDING_TABLE_BIT_KHR = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
#endif

#ifdef VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR
        VIDEO_ENCODE_DST_BIT_KHR = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
#endif
#ifdef VK_BUFFER_USAGE_VIDEO_ENCODE_SRC_BIT_KHR
        VIDEO_ENCODE_SRC_BIT_KHR = VK_BUFFER_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
#endif

#ifdef VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
        SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
#endif
#ifdef VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT
        RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
#endif
#ifdef VK_BUFFER_USAGE_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT
        PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT = VK_BUFFER_USAGE_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT,
#endif

#ifdef VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT
        MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT,
#endif
#ifdef VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT
        MICROMAP_STORAGE_BIT_EXT = VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT,
#endif

#ifdef VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_NV
        RAY_TRACING_BIT_NV = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_NV,
#endif

#ifdef VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_EXT
        SHADER_DEVICE_ADDRESS_BIT_EXT = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_EXT,
#endif
#ifdef VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR
        SHADER_DEVICE_ADDRESS_BIT_KHR = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
#endif
    };

    enum class EBufferSharingMode
    {
        EXCLUSIVE = VK_SHARING_MODE_EXCLUSIVE,
        CONCURRENT = VK_SHARING_MODE_CONCURRENT
    };

    class BufferBuilder
    {
    public:
        BufferBuilder()
        {
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.pNext = nullptr;
            info.flags = 0;

            // default values
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        BufferBuilder& setFlags(EBufferFlag flag)
        {
            info.flags = static_cast<VkBufferCreateFlagBits>(flag);
            return *this;
        }

        BufferBuilder& setFlags(VkBufferCreateFlags flags)
        {
            info.flags = flags;
            return *this;
        }

        BufferBuilder& addFlag(EBufferFlag flag)
        {
            info.flags |= static_cast<VkBufferCreateFlagBits>(flag);
            return *this;
        }

        BufferBuilder& setSize(VkDeviceSize size)
        {
            info.size = size;
            return *this;
        }

        BufferBuilder& setUsage(EBufferUsage usage)
        {
            info.usage = static_cast<VkBufferUsageFlagBits>(usage);
            return *this;
        }

        BufferBuilder& setUsage(VkBufferUsageFlags usage)
        {
            info.usage = usage;
            return *this;
        }

        BufferBuilder& addUsage(EBufferUsage usage)
        {
            info.usage |= static_cast<VkBufferUsageFlagBits>(usage);
            return *this;
        }

        BufferBuilder& setSharingMode(EBufferSharingMode sharingMode)
        {
            info.sharingMode = static_cast<VkSharingMode>(sharingMode);
            return *this;
        }

        // Make the created buffer's memory exportable to an external API (CUDA interop).
        // Chains a VkExternalMemoryBufferCreateInfo onto info.pNext. The chained struct is
        // a builder member, so it stays alive until build() (called on this live builder).
        BufferBuilder& setExternalMemoryHandleTypes(VkExternalMemoryHandleTypeFlags types)
        {
            ext_mem_ci_.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
            ext_mem_ci_.handleTypes = types;
            ext_mem_ci_.pNext = info.pNext; // preserve any existing chain
            info.pNext = &ext_mem_ci_;
            return *this;
        }

        Buffer build(VkDevice device, VkAllocationCallbacks* allocator = nullptr) const
        {
            return Buffer(device, info, allocator);
        }

    private:
        /*
        typedef struct VkBufferCreateInfo {
            VkStructureType        sType;
            const void*            pNext;
            VkBufferCreateFlags    flags;
            VkDeviceSize           size;
            VkBufferUsageFlags     usage;
            VkSharingMode          sharingMode;
            uint32_t               queueFamilyIndexCount;
            const uint32_t*        pQueueFamilyIndices;
        } VkBufferCreateInfo;
        */
        VkBufferCreateInfo info;
        VkExternalMemoryBufferCreateInfo ext_mem_ci_{}; // chained by setExternalMemoryHandleTypes
    };
}

#pragma once

#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    // Prepared once at attach. ResourceRegistry retains the instance until the Context's final ticket retires.
    struct StorageBufferBinding final
    {
        BufferAllocation* buffer;
        void* use;
        void (*submitted)(void*, SubmissionTicket) noexcept;
    };

    struct StorageBindingOps final
    {
        RenderResult<StorageBufferBinding> (*buffer)(void*, VkDeviceSize, VkBufferUsageFlags) noexcept;
    };
}

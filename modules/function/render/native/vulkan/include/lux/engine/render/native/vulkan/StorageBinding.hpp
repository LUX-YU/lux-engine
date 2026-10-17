#pragma once

#include <array>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    struct NativeResourcePlan;
    class DeviceSession;

    // Stable Context-owned binding. The professional reference protects future uses; its last-use
    // field is written directly after submit. No Storage lookup or dynamic dispatch occurs per draw.
    struct PreparedStorageBinding final
    {
        BufferAllocation* buffer = nullptr;
        VkDescriptorSet descriptors = VK_NULL_HANDLE;
        std::array<float, 4> parameters{};
        bool provides_parameters = false;
        SubmissionRequirements* last_use = nullptr;
        std::unique_ptr<void, void (*)(void*) noexcept> reference{nullptr, [](void*) noexcept {}};
    };

    struct StorageBindingOps final
    {
        RenderResult<void> (*prepare)(void*, const NativeResourcePlan&, const void*,
            PreparedStorageBinding&) noexcept;
        RenderResult<std::uint32_t> (*descriptor_count)(const DeviceSession&, const void* configuration) noexcept = nullptr;
    };
}

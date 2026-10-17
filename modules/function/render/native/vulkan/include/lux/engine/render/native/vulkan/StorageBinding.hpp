#pragma once

#include <lux/engine/render/data/ShaderBlock.hpp>
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
        std::span<const std::byte> parameters;
        const ShaderBlock* parameter_layout = nullptr; // Stable professional value layout, not physical placement.

        SubmissionRequirements* last_use = nullptr;
        void* update_context = nullptr;
        void (*updated)(void*) noexcept = nullptr; // Owner publication only; not a per-frame callback
        void publishUpdate() noexcept { if (updated) updated(update_context); }
        std::unique_ptr<void, void (*)(void*) noexcept> reference{nullptr, [](void*) noexcept {}};
    };

    struct StorageBindingOps final
    {
        RenderResult<void> (*prepare)(void*, const NativeResourcePlan&, const void*,
            PreparedStorageBinding&) noexcept;
    };
}

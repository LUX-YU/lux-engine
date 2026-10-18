#pragma once

#include <lux/engine/render/data/ShaderBlock.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    struct NativeResourcePlan;
    class DeviceSession;

    // Stable Context-owned binding. Resources hold physical content; reference owns an update subscription.
    // Native dependencies are merged after submit without Storage lookup or per-draw dynamic dispatch.
    struct PreparedStorageBinding final
    {
        Buffer* buffer = nullptr;
        VkDescriptorSet descriptors = VK_NULL_HANDLE;
        std::span<const std::byte> parameters;
        const ShaderBlock* parameter_layout = nullptr; // Stable professional value layout, not physical placement.

        // Immutable physical content retained by submitted snapshots, independent of the update subscription below.
        std::shared_ptr<const void> resources;

        SubmissionRequirements* last_use = nullptr;
        void* update_context = nullptr;
        void (*updated)(void*) noexcept = nullptr; // Owner publication only; not a per-frame callback

        void publishUpdate() noexcept
        {
            if (updated)
            {
                updated(update_context);
            }
        }

        std::unique_ptr<void, void (*)(void*) noexcept> reference{nullptr, [](void*) noexcept {}};
    };

    struct StorageBindingOps final
    {
        RenderResult<void> (*prepare)(void*, const NativeResourcePlan&, const void*, PreparedStorageBinding&) noexcept;
    };
} // namespace lux::render::vulkan

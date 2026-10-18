#pragma once

#include <lux/engine/render/data/ShaderBlock.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    struct NativeResourcePlan;
    class DeviceSession;

    // Stable Context-owned binding. Resources hold physical content; reference owns an update subscription.
    // Publication marks its Context dirty; the Owner prepares changed bindings before recording.
    struct PreparedStorageBinding final
    {
        Buffer* buffer = nullptr;
        VkDeviceSize buffer_offset = 0;
        std::span<const std::byte> parameters;
        const ShaderBlock* parameter_layout = nullptr; // Stable professional value layout, not physical placement.

        // Changed CPU data consumed at the next recording safe point. Empty on unchanged frames.
        // The Storage owns these bytes; native recording copies them into the command buffer.
        std::span<const std::byte> pending_bytes;

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

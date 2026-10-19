#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <vulkan/vulkan.h>

namespace lux::render::vulkan
{
    class DeviceSession;
    class ExecutionContext;

    // Actual Feature-derived per-attachment data. This is neither an async OperationState nor
    // a Storage binding owner. Stable frames call neither function; only invalidated data is prepared/recorded.
    struct ContextData final
    {
        std::unique_ptr<void, void (*)(void*) noexcept> value{nullptr, [](void*) noexcept {}};
        RenderResult<void> (*prepare)(void*) noexcept = nullptr;
        void (*record)(void*, VkCommandBuffer) noexcept = nullptr;
        bool dirty = false;
    };

    // Native-only cold factory contributed by a Feature. The existing GraphResourceId identifies
    // its output; no new resource table, identifier domain or author graph is introduced.
    struct ContextContribution final
    {
        GraphResourceId resource;
        ConfigValue configuration;
        RenderResult<
            ContextData> (*create)(DeviceSession&, ExecutionContext&, std::uint32_t, GraphResourceId, const void*) noexcept =
            nullptr;
    };
} // namespace lux::render::vulkan

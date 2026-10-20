#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <vulkan/vulkan.h>

namespace lux::render::detail
{
    class DeviceSession;
    class ExecutionContext;
    class UploadContext;

    // Actual Feature-derived per-attachment data. This is neither an async OperationState nor
    // a Storage binding owner. Native data decides whether the selected physical slice needs work.
    struct FeatureFrameWork final
    {
        enum class EPreparation : std::uint8_t
        {
            READY,
            RECORD,
            WAITING
        };

        std::unique_ptr<void, void (*)(void*) noexcept> value{nullptr, [](void*) noexcept {}};
        RenderResult<EPreparation> (*prepare)(void*, std::uint32_t, UploadContext&) noexcept = nullptr;
        void (*record)(void*, VkCommandBuffer, std::uint32_t) noexcept = nullptr;
    };

    // Native-only cold factory contributed by a Feature. The existing GraphResourceId identifies
    // its output; no new resource table, identifier domain or author graph is introduced.
    struct FeatureFrameContribution final
    {
        GraphResourceId resource;
        ConfigValue configuration;
        RenderResult<
            FeatureFrameWork> (*create)(DeviceSession&, ExecutionContext&, GraphResourceId, const void*) noexcept =
            nullptr;
    };
} // namespace lux::render::detail

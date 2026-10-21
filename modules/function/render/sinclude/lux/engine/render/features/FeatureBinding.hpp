#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>
#include <optional>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DeviceSession;
    class ResourceUploader;
    class FrameExecution;

    // Actual Feature-derived per-attachment data. This is neither an async OperationState nor
    // a Storage binding owner. Native data decides whether the selected physical slice needs work.
    struct FeatureBinding final
    {
        enum class EPreparation : std::uint8_t
        {
            READY,
            RECORD,
            WAITING
        };

        struct Preparation final
        {
            EPreparation state = EPreparation::READY;
            std::optional<ResourceInput> changed_resource;
            std::span<const ResourceUploader::BufferCopy> copies;
        };

        std::unique_ptr<void, void (*)(void*) noexcept> value{nullptr, [](void*) noexcept {}};
        RenderResult<Preparation> (*prepare)(void*, std::uint32_t, ResourceUploader&) noexcept = nullptr;
        GraphResourceId resource;
        std::span<const ResourceUploader::BufferCopy> prepared_copies;
    };

    // Native-only cold factory contributed by a Feature. The existing GraphResourceId identifies
    // its output; no new resource table, identifier domain or author graph is introduced.
    struct FeatureFrameContribution final
    {
        using Create = RenderResult<FeatureBinding> (*)(
            DeviceSession&,
            FrameExecution&,
            ResourceRegistry&,
            std::span<const StorageId>,
            const void*
        ) noexcept;

        GraphResourceId resource;
        ConfigValue configuration;
        Create create = nullptr;
    };
} // namespace lux::render::detail

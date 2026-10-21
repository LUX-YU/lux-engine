#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DeviceSession;
    class ResourceUploader;
    class FrameExecution;

    struct FeatureFrameInput final
    {
        std::uint32_t slot;
        std::span<const std::byte> parameters;
        std::span<const ResourceInput> resources;
    };

    struct ResourceUpdate final
    {
        GraphResourceId resource;
        ResourceInput input;
    };

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
            std::span<const ResourceUpdate> resources;
            std::span<const ResourceUploader::BufferCopy> copies;
        };

        std::unique_ptr<void, void (*)(void*) noexcept> value{nullptr, [](void*) noexcept {}};
        RenderResult<Preparation> (*prepare)(void*, const FeatureFrameInput&, ResourceUploader&) noexcept = nullptr;
        std::vector<GraphResourceId> inputs;
        std::vector<ResourceInput> resolved_inputs;
        std::span<const ResourceUploader::BufferCopy> prepared_copies;
        std::span<const ResourceUpdate> initial_resources;
    };

    // Native-only cold factory contributed by a Feature. The existing GraphResourceId identifies
    // its output; no new resource table, identifier domain or author graph is introduced.
    struct FeatureFrameContribution final
    {
        using Create = RenderResult<
            FeatureBinding> (*)(DeviceSession&, FrameExecution&, ResourceRegistry&, std::span<const StorageId>, const void*) noexcept;

        std::vector<GraphResourceId> outputs;
        ConfigValue configuration;
        Create create = nullptr;
        std::vector<GraphResourceId> inputs;
        std::uint32_t parameter_bytes = 0;
    };
} // namespace lux::render::detail

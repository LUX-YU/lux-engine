#pragma once

#include <array>

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/storage/StorageBinding.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DeviceSession;
    class ResourceUploader;
    class FrameExecution;
    class RenderGraphInstance;
    class CompiledPrograms;

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

    struct FeatureFrameContribution;

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

        using PrepareResources =
            RenderResult<Preparation> (*)(void*, const FeatureFrameInput&, ResourceUploader&) noexcept;

        struct RasterPreparation final
        {
            PrepareResources prepare_resources;
            bool (*requires_programs)(const void*) noexcept;
            exec::task<RenderResult<void>> (*prepare_programs)(void*, execution::CpuScheduler) noexcept;
        };

        using VPreparation = std::variant<PrepareResources, RasterPreparation>;
        std::unique_ptr<void, void (*)(void*) noexcept> value{nullptr, [](void*) noexcept {}};
        VPreparation preparation;
        // Effect declarations are frozen before any Attachment borrows this address.
        const FeatureFrameContribution* definition = nullptr;
        std::vector<ResourceInput> resolved_inputs;
        std::span<const ResourceUploader::BufferCopy> prepared_copies;
        std::span<const ResourceUpdate> initial_resources;
        // Null preserves true per-frame preparation (camera/light-derived values, for example).
        // A producer revision allows each physical slice to skip already prepared derived data.
        const std::uint64_t* preparation_revision = nullptr;
        std::array<std::uint64_t, 3> applied_preparation{};
    };

    // A cold factory creates imported resources or prepares state for its own live passes.
    // Pass attachments remain Graph-owned; preparing draws does not transfer image ownership.
    struct FeatureFrameContribution final
    {
        using CreateResult = RenderResult<FeatureBinding>;
        using Create = CreateResult (*)(
            DeviceSession&,
            FrameExecution&,
            ResourceRegistry&,
            RenderGraphInstance&,
            CompiledPrograms&,
            std::span<const StorageId>,
            const void*
        ) noexcept;

        std::vector<GraphResourceId> outputs;
        ConfigValue configuration;
        Create create = nullptr;
        std::vector<GraphResourceId> inputs;
        std::uint32_t parameter_bytes = 0;
        std::uint32_t parameter_group = 0, parameter_offset = 0;
        std::vector<GraphPassId> passes;
    };
} // namespace lux::render::detail

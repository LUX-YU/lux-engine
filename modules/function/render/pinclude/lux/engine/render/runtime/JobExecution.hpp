#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/FinalBufferVersion.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class DeviceEntry;
    struct EffectDeviceVariant;
    struct GraphBindingTag;
    class ViewParameterStorage;
    struct DeviceVariantTag;
    using DeviceVariantId = cxx::SlotKey<DeviceVariantTag>;
}

namespace lux::render::runtime_detail
{
    struct BoundStorageProjection final
    {
        ResourceInstanceId storage;
        std::uint32_t projection;
        GraphResourceId resource;
        std::optional<GraphBufferRange> output_range;
        std::uint32_t canonical_queue = kGraphInvalidIndex;
        cxx::SlotKey<detail::GraphBindingTag> subscription{};
        std::optional<FinalBufferVersion> publication;
        std::span<const detail::FinalBufferUse> uses;
        std::span<const detail::FinalBufferExport> exports;
    };

    struct JobExecution final
    {
        JobExecution(detail::DeviceEntry&, const detail::EffectDeviceVariant&, TargetId, detail::TargetOutput) noexcept;
        ~JobExecution() noexcept;
        JobExecution(const JobExecution&) = delete;
        JobExecution& operator=(const JobExecution&) = delete;

        const EffectId effect;
        std::vector<ResourceInstanceId> resource_ids;
        std::vector<ResourceInstanceId> gpu_resources;
        DeviceSessionId device;
        detail::DeviceVariantId variant;
        TargetId target;
        const compiled::RenderGraph& graph;
        const detail::RenderGraphExecutable& executable;
        detail::RenderGraphInstance instance;
        std::uint32_t pending_uses = 0;
        struct ProgramPreparation final
        {
            std::vector<std::uint64_t> revisions;
            std::optional<stdexec::inplace_stop_source> stop;
            RenderError failure;
        };
        ProgramPreparation programs;

        [[nodiscard]] RenderError programError() const noexcept
        {
            if (programs.failure.type && instance.matchesProgramRevisions(programs.revisions))
                return programs.failure;
            return {};
        }

        [[nodiscard]] bool programsReady() const noexcept
        {
            return !programs.stop && !instance.requiresProducerPrograms();
        }

        FrameLayout input_layout;
        // Qualified once at assembly. These identities also define which exclusive commands
        // conflict with a live Run, including resources used only for GPU bindings.
        [[nodiscard]] std::span<const ResourceInstanceId> retainedResources() const & noexcept
        {
            return resource_ids;
        }
        // Each registered Resource type owns exactly one FrameDataCall.
        struct Input final
        {
            ResourceInstanceId identity;
            ResourceTypeId call_slot;
            struct ColumnRange final
            {
                std::uint32_t first, count;
                bool operator==(const ColumnRange&) const noexcept = default;
            };
            ColumnRange columns;
        };
        std::vector<Input> inputs;
        std::vector<std::uint32_t> input_columns;

        [[nodiscard]] std::span<const std::uint32_t> columnsFor(const Input& input) const & noexcept
        {
            return std::span(input_columns).subspan(input.columns.first, input.columns.count);
        }
        std::span<const std::uint32_t> columnsFor(const Input&) const && = delete;

        detail::TargetReservation storage_start;
        std::vector<BoundStorageProjection> storage_bindings;
        std::size_t product_count = 0;
        [[nodiscard]] std::span<const BoundStorageProjection> productUses() const & noexcept
        {
            return std::span{storage_bindings}.first(product_count);
        }
        std::span<const BoundStorageProjection> productUses() const && = delete;

        struct TargetImage final
        {
            TargetId target;
            GraphResourceId resource;
        };

        std::vector<TargetImage> target_images;
        void selectViewParameters() noexcept;
        const detail::ViewParameterStorage* view_parameters = nullptr;
        std::uint64_t view_revision = 0;
    };
} // namespace lux::render::runtime_detail

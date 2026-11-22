#pragma once

#include <lux/engine/render/runtime/ExecutionBindingId.hpp>
#include <lux/engine/render/runtime/RasterProgramInputs.hpp>
#include <lux/engine/render/runtime/JobCpuPreparation.hpp>

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/core/FinalBufferVersion.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <algorithm>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class JobGpuResources;
    struct GraphBindingTag;
    class FrameOperation;
    class SubmissionOperation;
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
        struct Installation final
        {
            DeviceSessionId device;
            EffectId effect;
            TargetId target;
            detail::ExecutionBindingId identity;
        };
        LUX_RENDER_PUBLIC JobExecution(
            detail::JobGpuResources&,
            const compiled::RenderGraph&,
            Installation,
            detail::TargetOutput
        ) noexcept;
        LUX_RENDER_PUBLIC ~JobExecution() noexcept;
        JobExecution(const JobExecution&) = delete;
        JobExecution& operator=(const JobExecution&) = delete;

        const EffectId effect;
        std::vector<ResourceInstanceId> resource_ids;
        std::vector<ResourceInstanceId> gpu_resources;
        DeviceSessionId device;
        TargetId target;
    private:
        friend class detail::FrameOperation;
        friend class detail::SubmissionOperation;
        const compiled::RenderGraph& graph_;
        detail::TargetOutput output_;
        detail::JobGpuResources& gpu_;
        RenderError submission_error_;
        std::vector<std::uint8_t> condition_values_, binding_values_;
        [[nodiscard]] bool conditionEnabled(const detail::FrameResources&, PassCondition) const noexcept;
        void acceptedBatch(std::uint32_t slot, std::uint32_t batch, detail::SubmissionPoint point) noexcept;
        void submitted(std::uint32_t slot) noexcept;
        // Stable CPU rows outlive their preparation borrows. DeviceEntry owns the native resources.
        std::vector<detail::FrameResources> frames_;
        std::vector<detail::HistoryState> histories_;

    public:
        ExecutionProgress progress;
        RasterViewport viewport;
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> initialize() noexcept;
        [[nodiscard]] RenderResult<void> bindPredicates(std::vector<std::uint8_t>) noexcept;
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> setConditions(std::span<const std::uint8_t>) noexcept;
        [[nodiscard]] bool frozenCondition(std::uint32_t slot, PassCondition) const noexcept;
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<bool> freezeFrame(std::uint32_t slot) noexcept;

        detail::RasterProgramInputs programs;
        detail::JobCpuPreparation preparation;
        detail::RenderGraphInstance instance;
        [[nodiscard]] const compiled::RenderGraph& graph() const & noexcept { return graph_; }
        const compiled::RenderGraph& graph() const && = delete;

        void settlePrograms(detail::RasterProgramUpdate& update) noexcept;

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

        const detail::ExecutionBindingId binding_identity;
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

        [[nodiscard]] bool bindsTarget(TargetId id) const noexcept
        {
            return target == id || std::ranges::find(target_images, id, &TargetImage::target) != target_images.end();
        }
    };
} // namespace lux::render::runtime_detail

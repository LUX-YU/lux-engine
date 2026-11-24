#pragma once

#include <lux/engine/render/runtime/JobBinding.hpp>
#include <lux/engine/render/resource/InstalledProjection.hpp>
#include <lux/engine/render/runtime/RenderEffect.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/runtime/InstalledFrameResource.hpp>


#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/effect/EffectId.hpp>
#include <lux/engine/render/graph/Ids.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/runtime/TargetId.hpp>
#include <lux/engine/render/runtime/Execution.hpp>
#include <lux/engine/render/graph/RasterViewport.hpp>
#include <lux/engine/render/core/FinalBufferVersion.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/target/TargetOutput.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <algorithm>
#include <memory>
#include <vector>

namespace lux::render { class RasterProgramSet; }

namespace lux::render::detail
{
    struct BoundConstantBuffer;
    class JobGpuResources;
    struct ResizeResources;
    class SubmissionOperation;
}

namespace lux::render::runtime_detail
{
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
        std::vector<detail::InstalledFrameResource> frame_resources;
        // Construction dependencies excluding the direct Resource IDs retained by the Job.
        std::vector<ResourceInstanceId> indirect_resources;
        std::uint32_t useFrameResource(detail::InstalledFrameResource) noexcept;
        struct DrawListSource final
        {
            const detail::CompiledRasterStream& stream;
            const void* owner;
            detail::StorageBindingOps::ReadDrawList read;
        };
        std::vector<DrawListSource> draw_list_sources;
        [[nodiscard]] RenderResult<void> bindPublication(const detail::CompiledRasterStream&, const RasterProgramSet&) noexcept;
        // One Resource identity per compiled frame-view operand, in declaration order.
        std::vector<std::uint32_t> view_sources;
        std::vector<ResourceInstanceId> draw_resource_ids;
        std::vector<std::span<const std::uint64_t>> buffer_extents;
        // Set on installation. Native-only executions have no Effect inputs.
        const detail::EffectDeviceGraph* device_graph = nullptr;
        DeviceSessionId device;
        TargetId target;
    private:
        friend struct detail::CpuDrawStorage;
        friend struct DeviceRound;
        friend class detail::SubmissionOperation;
        friend struct detail::ResizeResources;
        detail::TargetOutput output_;
        detail::JobGpuResources& gpu_;
        RenderError submission_error_;
        std::vector<std::uint8_t> condition_values_, binding_values_;
        [[nodiscard]] bool conditionEnabled(PassCondition, const detail::FrameResources* frozen = nullptr) const noexcept;
        void acceptedBatch(std::uint32_t slot, std::uint32_t batch, detail::SubmissionPoint point) noexcept;
        void submitted(std::uint32_t slot) noexcept;
        void freezeConditions(detail::FrameResources&) const noexcept;
        // Stable CPU rows outlive their preparation borrows. DeviceEntry owns the native resources.
    public:
        detail::InstalledGraphBindings frame_bindings;

    private:
        struct ProgramBinding final
        {
            const detail::CompiledRasterStream* stream;
            detail::ReadRasterPrograms read;
            std::uint32_t view = kGraphInvalidIndex;
            std::uint64_t revision = 0;
        };
        // Program selections outlive the algorithm workspaces borrowing their published draw mappings.
        std::vector<ProgramBinding> program_bindings_;
        std::vector<detail::CpuDrawStorage> cpu_draws_;
        std::vector<detail::HistoryState> histories_;
        std::vector<extension_detail::AlgorithmWorkspace> algorithm_workspaces_;
        std::vector<std::byte> inline_bytes_;
        std::uint64_t draw_selection_ = 0;
        [[nodiscard]] RenderResult<void> selectPublishedPrograms(const extension_detail::SelectedResourceViews&) noexcept;
        [[nodiscard]] RenderResult<void>
        selectFrameInputs(
            std::uint32_t slot,
            std::span<detail::PreparedBufferInput>,
            const extension_detail::SelectedResourceViews&,
            std::vector<detail::PreparedBufferUpload>&
        ) noexcept;
        [[nodiscard]] RenderResult<void>
        runFrameAlgorithms(std::uint32_t slot, std::span<detail::PreparedBufferInput>, const extension_detail::SelectedResourceViews&) noexcept;

    public:
        ExecutionProgress progress;
        RasterViewport viewport;
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> initialize() noexcept;
        [[nodiscard]] RenderResult<void> bindPredicates(std::vector<std::uint8_t>) noexcept;
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> setConditions(std::span<const std::uint8_t>) noexcept;
        [[nodiscard]] bool passEnabledThisRun(std::uint32_t pass) const noexcept;
        [[nodiscard]] bool writesTargetThisRun() const noexcept;
        [[nodiscard]] bool frozenCondition(std::uint32_t slot, PassCondition) const noexcept;
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<bool>
        freezeFrame(std::uint32_t slot, std::vector<detail::RasterDrawInput>& draws) noexcept;

        [[nodiscard]] RenderResult<void> bindFrameAlgorithms(std::span<const ResolvedResource>) noexcept;
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void>
        invalidateHistory(std::span<const GraphResourceId> previous) noexcept;
        void releaseOutputReferences() noexcept;
        [[nodiscard]] const TargetDesc& targetDescription() const & noexcept { return output_.description; }
        const TargetDesc& targetDescription() const && = delete;
        [[nodiscard]] detail::FrameResources& frame(std::uint32_t slot) & noexcept { return frame_bindings.frames[slot]; }
        [[nodiscard]] const detail::FrameResources& frame(std::uint32_t slot) const & noexcept { return frame_bindings.frames[slot]; }
        const detail::FrameResources& frame(std::uint32_t) const && = delete;
        [[nodiscard]] const compiled::RenderGraph& graph() const & noexcept { return frame_bindings.graph; }
        const compiled::RenderGraph& graph() const && = delete;

        [[nodiscard]] ResourceSet* privateResources() const noexcept { return gpu_.private_resources.get(); }
        [[nodiscard]] ResourceSet& privateResources(ResourceSet& parent) noexcept;
        [[nodiscard]] LUX_RENDER_PUBLIC std::uint32_t frameCount() const noexcept;
        [[nodiscard]] LUX_RENDER_PUBLIC detail::ResourceInput
        resourceInput(GraphResourceId, std::uint32_t slot) const noexcept;
        LUX_RENDER_PUBLIC void bindImage(GraphResourceId, std::uint32_t slot, nri::Texture&) noexcept;
        [[nodiscard]] LUX_RENDER_PUBLIC bool dumpResources(DumpSink) const noexcept;


        const detail::ExecutionBindingId binding_identity;
        // Subscription keys end with this installation. Product pins last from admission through submission settlement.
        std::vector<ProjectionSubscription> projection_subscriptions;
        std::vector<BufferProductBinding> buffer_products;

        struct TargetImage final
        {
            TargetId target;
            const detail::TargetImageSlot& operand;
            EProductRead read;
        };

        std::vector<TargetImage> target_images;

        [[nodiscard]] bool readsTarget(TargetId id) const noexcept
        {
            return std::ranges::find(target_images, id, &TargetImage::target) != target_images.end();
        }

        [[nodiscard]] bool bindsTarget(TargetId id) const noexcept
        {
            return target == id || readsTarget(id);
        }
    };
} // namespace lux::render::runtime_detail

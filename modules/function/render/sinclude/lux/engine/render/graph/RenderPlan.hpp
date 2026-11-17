#pragma once

#include <lux/engine/core/execution/Scheduler.hpp>
#include <exec/task.hpp>
#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/device/SubmissionQueue.hpp>
#include <lux/engine/render/extensions/graph/Diagnostics.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/extensions/graph/Work.hpp>
#include <memory>

namespace lux::render::detail
{
    struct ShaderBindingPlan;
    class CompiledPrograms;
    class DeviceSession;
    class RenderPlan;
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpPlan(const RenderPlan&, EGraphDumpFormat, DumpSink) noexcept;

    enum class EImageViewTarget : std::uint8_t
    {
        DESCRIPTOR,
        COLOR,
        DEPTH,
        COLOR_RESOLVE,
        DEPTH_RESOLVE,
        CLEAR
    };

    struct ImageViewUse final
    {
        std::uint32_t pass, index;
        EImageViewTarget target;
        GraphImageRange range;
    };

    struct CompiledResource final
    {
        std::string key;
        VGraphResourceDesc description;
        EGraphOrigin origin;
        nri::TextureUsageBits image_usage = nri::TextureUsageBits::NONE;
        nri::BufferUsageBits buffer_usage = nri::BufferUsageBits::NONE;
        bool used = false;
        bool memory_used = false;
        bool target_sized = false;
        bool shared_table = false;
        bool sampled_target_input = false;
        std::uint32_t history = kGraphInvalidIndex;
        // Immutable reverse indexes shared by every Context frame slice.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> barrier_uses, descriptor_uses;
        std::vector<std::uint32_t> final_barrier_uses;
        std::vector<ImageViewUse> image_views;
        std::vector<std::uint32_t> extent_uses;
    };

    enum class ETransitionBoundary : std::uint8_t
    {
        PASS,
        RESTORE_IMPORTED,
        EXPORT
    };

    struct ImageTransition final
    {
        std::uint32_t resource;
        nri::TextureBarrierDesc native;
        GraphPassId previous;
        ETransitionBoundary boundary = ETransitionBoundary::PASS;
    };

    struct BufferTransition final
    {
        std::uint32_t resource;
        nri::BufferBarrierDesc native;
        GraphPassId previous;
        ETransitionBoundary boundary = ETransitionBoundary::PASS;
    };

    struct BarrierBatch final
    {
        std::vector<ImageTransition> images;
        std::vector<BufferTransition> buffers;
    };

    enum class ERenderingBoundary : std::uint8_t
    {
        NON_RASTER,
        FIRST_RASTER,
        ATTACHMENT_OR_LOAD_STORE,
        RESOURCE_DEPENDENCY,
        CONTINUATION
    };

    struct VertexSource final
    {
        GraphResourceId resource;
        std::uint32_t binding;
        std::uint64_t offset;
        std::uint32_t stride;
        bool operator==(const VertexSource&) const noexcept = default;
    };

    // Recording values contain neither author names, Shader artifacts nor pipeline construction state.
    struct IndexedListCommands final
    {
        ResourceVersion index_resource;
        std::uint64_t index_offset;
        EIndexType type;
        std::uint32_t selection_offset, data_index;
        bool partial_parameters = false;
    };

    struct RasterListCommands final
    {
        std::uint32_t data_index;
        RasterPassTemplate description;
    };

    using VRasterDraw =
        std::variant<DrawVertices, DrawIndexed, DrawIndexedIndirect, IndexedListCommands, RasterListCommands>;

    struct RasterCommands final
    {
        [[nodiscard]] RenderResult<void>
        record(const PassRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&) const noexcept;

        // Fixed raster program. Dynamic raster lists carry the program on each draw packet.
        std::vector<VertexSource> vertices;
        VRasterDraw draw;
    };

    struct ComputeCommands final
    {
        [[nodiscard]] RenderResult<void>
        record(const PassRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&) const noexcept;

        VDispatchArguments dispatch;
    };

    struct TransferCommands final
    {
        std::vector<VTransferOperation> operations;

        [[nodiscard]] RenderResult<void>
        record(const PassRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&) const noexcept;
    };

    static_assert(sizeof(RasterCommands) <= 768 && sizeof(ComputeCommands) <= 768 && sizeof(TransferCommands) <= 768);

    struct CompiledRasterScope final : RasterScope
    {
        bool exclusive = false;
        std::array<float, 2> depth_bias{};
    };

    struct CompiledPass final
    {
        [[nodiscard]] const CompiledRasterScope* rasterScope() const noexcept
        {
            return rendering ? &*rendering : nullptr;
        }

        [[nodiscard]] CompiledRasterScope* rasterScope() noexcept
        {
            return rendering ? &*rendering : nullptr;
        }

        PassEncoding work;
        std::optional<CompiledRasterScope> rendering;
        EGraphPassKind kind = EGraphPassKind::TRANSFER;
        nri::Pipeline* pipeline = nullptr;
        BarrierBatch before;
        std::uint32_t source_pass = kGraphInvalidIndex;
        std::uint32_t submission = kGraphInvalidIndex;
        std::uint32_t rendering_group = kGraphInvalidIndex;
        ERenderingBoundary rendering_boundary = ERenderingBoundary::NON_RASTER;
        std::uint32_t condition_region = kGraphInvalidIndex;
        EQueueRole queue = EQueueRole::GRAPHICS;
        std::size_t parameter_offset = 0;
        std::size_t parameter_size = 0;
    };

    // A linear, non-nested interval. Both paths execute its compiled transitions;
    // only the true path records work. The last transition establishes the common exit state.
    struct ConditionalRegion final
    {
        PassCondition predicate;
        std::uint32_t first_pass = 0;
        std::uint32_t pass_count = 1;
    };

    struct RenderingGroup final
    {
        std::uint32_t first_pass = 0;
        std::uint32_t pass_count = 1;
        std::vector<RasterAttachment> colors;
        std::optional<RasterAttachment> depth;
    };

    struct RecordingProgram final
    {
        std::vector<CompiledPass> passes;
        std::vector<RenderingGroup> groups;
        std::vector<ConditionalRegion> conditions;
        BarrierBatch after;
    };

    struct HistoryPlan final
    {
        HistoryDescription description;
        // Compiled producer indexes, including disjoint writes composing the published version.
        std::vector<std::uint32_t> writers;
    };

    struct SubmissionDependency final
    {
        std::uint32_t producer;
        nri::StageBits stages = nri::StageBits::ALL;
    };

    struct SubmissionSuccessor final
    {
        std::uint32_t consumer;
        nri::StageBits stages = nri::StageBits::ALL;
    };

    struct SubmissionBatch final
    {
        EQueueRole queue = EQueueRole::GRAPHICS;
        std::uint32_t first_pass = 0;
        std::uint32_t pass_count = 0;
        std::vector<SubmissionDependency> dependencies;
        std::vector<SubmissionSuccessor> successors;
        std::vector<GraphResourceId> histories;
        std::uint32_t output_begin = kGraphInvalidIndex;
        bool reusable_recording = true;
    };

    struct ImportedAccess final
    {
        GraphResourceId resource;
        VGraphRange range;
        bool write = false;
        std::uint32_t batch = 0;
        // A later canonical-state restoration belongs to the same physical access interval.
        std::uint32_t release_batch = 0;
        bool whole_resource = false;
    };

    enum class ETransientAllocation : std::uint8_t
    {
        INDEPENDENT,
        REUSE_OBJECTS,
        ALIAS_MEMORY
    };

    enum class EResourceReuse : std::uint8_t
    {
        UNUSED,
        DYNAMIC_EXTENT,
        EXTERNAL,
        HISTORY,
        EXPORTED,
        CONDITIONAL,
        INITIAL_CONTENT,
        DISABLED,
        DISTINCT_DESCRIPTION,
        UNORDERED,
        REUSED
    };

    struct PhysicalResourceSlot final
    {
        GraphResourceId resource;
        std::array<std::uint32_t, 3> before{}, end{};
        std::array<std::uint32_t, 3> first_pass{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
        std::array<nri::AccessBits, 3> accesses{};
    };

    // Logical bindings borrow these unique physical slots; the instance owns one set per FIF.
    struct PhysicalResourcePlan final
    {
        ETransientAllocation allocation = ETransientAllocation::REUSE_OBJECTS;
        std::vector<PhysicalResourceSlot> slots;
        std::vector<std::uint32_t> resource_slots;
        std::vector<EResourceReuse> reasons;
    };

    // The execution candidate owns this immutable result; its graph instance borrows it until teardown.
    class LUX_RENDER_PUBLIC RenderPlan final
    {
    public:
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> parameterRegion(std::uint32_t) const noexcept;
        ~RenderPlan() noexcept;
        RenderPlan(RenderPlan&& other) noexcept;
        RenderPlan& operator=(RenderPlan&&) = delete;
        RenderPlan(const RenderPlan&) = delete;
        RenderPlan& operator=(const RenderPlan&) = delete;

        [[nodiscard]] nri::Device* device() const noexcept
        {
            return device_;
        }

        [[nodiscard]] const EffectOutputDesc& outputDescription() const noexcept
        {
            return output_description_;
        }

        [[nodiscard]] std::span<const CompiledResource> resources() const noexcept
        {
            return resources_;
        }

        [[nodiscard]] std::span<const CompiledPass> passes() const noexcept
        {
            return recording_.passes;
        }

        [[nodiscard]] std::uint32_t output() const noexcept
        {
            return output_;
        }

        // Unpublished plan, called once on RenderThread. Graph and Session outlive completion.
        // Workers compile frozen inputs; publication/references return to the caller's scheduler.
        [[nodiscard]] exec::task<RenderResult<void>>
        preparePrograms(DeviceSession&, const RenderGraph&, execution::CpuScheduler) noexcept;

        [[nodiscard]] bool compatible(const TargetDesc& target) const noexcept;

        [[nodiscard]] std::span<const SubmissionBatch> submissions() const noexcept
        {
            return submissions_;
        }

        [[nodiscard]] const PhysicalResourcePlan& physicalResources() const noexcept
        {
            return physical_resources_;
        }

    private:
        friend class RenderGraphInstance;
        friend class GraphCompiler;
        friend class FrameExecutor;
        friend class FrameExecution;
        friend LUX_RENDER_PUBLIC bool dumpPlan(const RenderPlan&, EGraphDumpFormat, DumpSink) noexcept;

        RenderPlan(nri::Device& device, EffectOutputDesc output) noexcept;
        std::unique_ptr<ShaderBindingPlan> descriptor_plan_;
        nri::Device* device_;
        EffectOutputDesc output_description_;
        CompiledPrograms* program_cache_ = nullptr;
        DeviceSession* encoding_owner_ = nullptr;
        std::vector<nri::Pipeline*> program_uses_;
        std::vector<nri::PipelineLayout*> layout_uses_;
        std::vector<CompiledResource> resources_;
        std::vector<HistoryPlan> histories_;
        std::uint32_t condition_count_ = 0;
        RecordingProgram recording_;
        std::vector<SubmissionBatch> submissions_;
        std::vector<ImportedAccess> imported_accesses_;
        PhysicalResourcePlan physical_resources_;
        std::vector<std::uint32_t> pass_indices_;

        std::uint32_t output_ = kGraphInvalidIndex;
        std::uint32_t output_first_batch_ = kGraphInvalidIndex;
        nri::StageBits output_wait_stages_ = nri::StageBits::ALL;
        std::array<std::uint32_t, 2> fixed_output_extent_{};
    };

} // namespace lux::render::detail

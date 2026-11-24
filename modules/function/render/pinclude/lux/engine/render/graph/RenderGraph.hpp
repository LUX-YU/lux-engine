#pragma once

#include <lux/engine/render/device/CompiledPrograms.hpp>

#include <lux/engine/core/execution/Scheduler.hpp>
#include <exec/task.hpp>
#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/graph/Diagnostics.hpp>
#include <lux/engine/render/graph/GraphError.hpp>
#include <lux/engine/render/graph/PassValues.hpp>
#include <lux/engine/render/graph/CommandPackets.hpp>
#include <memory>

#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <lux/engine/render/graph/TargetImageSlot.hpp>

namespace lux::render::compiled { class RenderGraph; }

namespace lux::render::detail
{
    struct CompiledPassSlot final
    {
        std::uint32_t index = kGraphInvalidIndex;
    };

    struct DynamicBuffer final
    {
        std::uint32_t resource;
        std::uint64_t minimum_bytes, maximum_bytes;
        bool upload, used;
    };

    struct GraphBindingPlan;
    struct RecordingDependencies;
    struct DescriptorAccess;
    class DeviceSession;
    class RenderGraphCompiler;
    class FrameExecutor;
    class SubmissionOperation;
    class NativeProgramBindings;
    struct CompiledRasterStream;
    struct PreparedRenderGraph;
    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpPlan(const PreparedRenderGraph&, EGraphDumpFormat, DumpSink) noexcept;

    enum class EImageViewTarget : std::uint8_t
    {
        DESCRIPTOR,
        COLOR,
        DEPTH,
        COLOR_RESOLVE,
        DEPTH_RESOLVE,
        CLEAR
    };

    struct ImageViewBinding final
    {
        std::uint32_t pass, index;
        EImageViewTarget target;
        GraphImageRange range;
    };

    struct CompiledResource final
    {
        VGraphResourceDesc description;
        EGraphOrigin origin;
        nri::TextureUsageBits image_usage = nri::TextureUsageBits::NONE;
        nri::BufferUsageBits buffer_usage = nri::BufferUsageBits::NONE;
        bool used = false;
        bool memory_used = false;
        bool target_sized = false;
        bool concurrent_image_read = false;
        bool shared_table = false;
        bool sampled_target_input = false;
        std::uint32_t history = kGraphInvalidIndex;
        std::uint32_t layer_slot = kGraphInvalidIndex;
        // Immutable reverse indexes shared by every Context frame slice.
        std::vector<std::size_t> barrier_uses;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> descriptor_uses;
        std::vector<std::uint32_t> exit_barrier_uses;
        std::vector<ImageViewBinding> image_views;
        std::vector<std::uint32_t> extent_uses;
    };

    enum class ETransitionBoundary : std::uint8_t
    {
        PASS,
        RESTORE_IMPORTED,
        RESTORE_TRANSIENT,
        EXPORT,
        PUBLISH_READS
    };

    struct ImageTransition final
    {
        std::uint32_t resource;
        nri::TextureBarrierDesc native;
        std::uint32_t previous_slot;
        ETransitionBoundary boundary = ETransitionBoundary::PASS;
    };

    struct BufferTransition final
    {
        std::uint32_t resource;
        nri::BufferBarrierDesc native;
        std::uint32_t previous_slot;
        ETransitionBoundary boundary = ETransitionBoundary::PASS;
        bool bind_initial_access = false;
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
        std::uint32_t buffer, binding;
        std::uint64_t offset;
        std::uint32_t stride;
        bool operator==(const VertexSource&) const noexcept = default;
    };

    // Recording values contain neither author names, Shader artifacts nor pipeline construction state.
    struct RasterListCommands final
    {
        std::uint32_t data_index;
        bool producer_stream = false;
    };

    struct IndexSource final
    {
        std::uint32_t address;
        nri::IndexType type;
    };

    struct RasterBufferAddress final
    {
        BufferLocation location;
        std::uint64_t bytes;
        std::uint32_t alignment;
    };

    struct IndexedDrawPacket final
    {
        IndexSource index;
        nri::DrawIndexedDesc draw;
    };

    struct IndirectDrawPacket final
    {
        IndexSource index;
        std::uint32_t arguments, count_buffer;
        std::uint32_t count, stride;
    };

    using VRasterDraw = std::variant<nri::DrawDesc, IndexedDrawPacket, IndirectDrawPacket, RasterListCommands>;

    struct RasterCommands final
    {
        // Fixed raster program. Dynamic raster lists carry the program on each draw packet.
        std::uint32_t first_vertex = 0, vertex_count = 0;
        VRasterDraw draw;
    };

    template <EGraphPassKind Kind>
    struct CommandSlot final
    {
        static constexpr EGraphPassKind kind = Kind;
        std::uint32_t index = kGraphInvalidIndex;
    };

    using RasterCommandSlot = CommandSlot<EGraphPassKind::GRAPHICS>;
    using ComputeCommandSlot = CommandSlot<EGraphPassKind::COMPUTE>;
    using TransferCommandSlot = CommandSlot<EGraphPassKind::TRANSFER>;
    using VCommandSlot = std::variant<RasterCommandSlot, ComputeCommandSlot, TransferCommandSlot>;

    struct CompiledRasterScope final : RasterScope
    {
        bool exclusive = false;
        nri::DepthBiasDesc depth_bias{};
        bool section_depth_bias = false;
        bool initialize_resolve_targets = false;
        bool reset_viewport = false;
    };

    struct FixedBindingDelta final
    {
        nri::BindPoint point = nri::BindPoint::INHERIT;
        std::uint8_t shared_sets = 0; // Bits select the frozen texture, sampler and buffer tables.
        bool pool = false, layout = false, pipeline = false, private_set = false, constants = false;
    };

    // A conditional region has a single exit. Consecutive skipped regions can select the exit
    // of an earlier join. Indices always refer to the same physical command buffer.
    struct BindingJoin final
    {
        std::uint32_t pass, enabled_predecessor, disabled_predecessor, disabled_join;
        FixedBindingDelta enabled, disabled;
    };

    struct SelectedBindingDelta final
    {
        std::uint32_t predecessor = kGraphInvalidIndex;
        FixedBindingDelta commands;
    };

    struct BarrierRange final
    {
        std::size_t first = 0, count = 0;

        template <class T>
        [[nodiscard]] std::span<T> select(std::span<T> rows) const noexcept
        {
            return rows.subspan(first, count);
        }
    };

    struct CompiledPass final
    {
        VCommandSlot commands;
        FixedBindingDelta fixed_bindings;
        std::uint32_t binding_join = kGraphInvalidIndex;
        BarrierRange entry_images, entry_buffers;
        std::uint32_t binding_slot = kGraphInvalidIndex;
        std::uint32_t unit = kGraphInvalidIndex;
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
        std::uint32_t first_pass;
        std::uint32_t pass_count;
        std::uint32_t input_slot;
        EConditionSource source;
        bool expected;
        std::uint32_t binding_join = kGraphInvalidIndex;
    };

    struct RenderingGroup final
    {
        std::uint32_t first_pass = 0;
        std::uint32_t pass_count = 1;
        std::vector<RasterAttachment> colors;
        std::optional<RasterAttachment> depth;
    };

    struct RecordingUnit final
    {
        std::uint32_t first_pass;
        std::uint32_t pass_count;
    };

    struct RecordingProgram final
    {
        [[nodiscard]] const CompiledRasterScope* rasterScope(const CompiledPass& pass) const noexcept
        {
            const auto* slot = std::get_if<RasterCommandSlot>(&pass.commands);
            return slot ? &raster_scopes[slot->index] : nullptr;
        }

        [[nodiscard]] const RasterCommands* rasterCommands(const CompiledPass& pass) const noexcept
        {
            const auto* slot = std::get_if<RasterCommandSlot>(&pass.commands);
            return slot ? &raster[slot->index] : nullptr;
        }

        [[nodiscard]] const ComputeCommands* computeCommands(const CompiledPass& pass) const noexcept
        {
            const auto* slot = std::get_if<ComputeCommandSlot>(&pass.commands);
            return slot ? &compute[slot->index] : nullptr;
        }

        [[nodiscard]] const TransferCommands* transferCommands(const CompiledPass& pass) const noexcept
        {
            const auto* slot = std::get_if<TransferCommandSlot>(&pass.commands);
            return slot ? &transfer[slot->index] : nullptr;
        }

        std::vector<CompiledPass> passes;
        std::vector<RecordingUnit> units;
        // Both raster tables are indexed by the same RasterCommandSlot.
        std::vector<CompiledRasterScope> raster_scopes;
        std::vector<RasterCommands> raster;
        std::vector<VertexSource> raster_vertices;
        std::vector<RasterBufferAddress> raster_addresses;
        std::vector<ComputeCommands> compute;
        std::vector<TransferCommands> transfer;
        TransferProgram transfers;
        std::vector<RenderingGroup> groups;
        std::vector<ConditionalRegion> conditions;
        std::vector<BindingJoin> binding_joins;
        BarrierBatch before, after;
        [[nodiscard]] std::span<const ImageTransition> imageBarriers(const CompiledPass& pass) const & noexcept
        {
            return pass.entry_images.select(std::span{before.images});
        }
        [[nodiscard]] std::span<const BufferTransition> bufferBarriers(const CompiledPass& pass) const & noexcept
        {
            return pass.entry_buffers.select(std::span{before.buffers});
        }
        std::span<const ImageTransition> imageBarriers(const CompiledPass&) const && = delete;
        std::span<const BufferTransition> bufferBarriers(const CompiledPass&) const && = delete;

        // Legacy textures retain their state across FIF reuse. Close each physical subresource at its last use.
        std::vector<ImageTransition> transient_exits;
        std::vector<std::uint32_t> transient_exit_offsets;
        // Whole-buffer write-to-read transitions executed before the producer signals its queue.
        std::vector<BufferTransition> buffer_read_releases;
        std::vector<std::uint32_t> buffer_release_offsets;
    };

    template <class Recorder>
    decltype(auto) recordBlock(const RecordingProgram& program, VCommandSlot block, Recorder recorder) noexcept
    {
        return std::visit([&](auto slot) noexcept -> decltype(auto)
        {
            if constexpr (slot.kind == EGraphPassKind::GRAPHICS) return recorder(program.raster[slot.index], slot.index);
            else if constexpr (slot.kind == EGraphPassKind::COMPUTE) return recorder(program.compute[slot.index], slot.index);
            else return recorder(program.transfer[slot.index], slot.index);
        }, block);
    }

    struct HistoryPlan final
    {
        using VViewDescription = std::variant<nri::BufferViewDesc, nri::TextureViewDesc>;

        struct View final
        {
            std::uint32_t pass, index;
            EImageViewTarget target;
            bool previous;
            VViewDescription description;
            std::uint32_t binding = 0, element = 0;
            bool has_fallback = false;
        };

        HistoryDescription description;
        std::vector<View> views;
        // Publication is proved complete for every selected path during range analysis.
    };

    // Device qualifications for dynamic Raster operands, fixed when native programs are linked.
    struct RasterDrawLimits final
    {
        std::uint64_t uniform_alignment = 1;
        std::uint32_t indirect_count = 1;
        bool multi_draw = false, indirect_count_buffer = false;
    };

    struct ResourceUseStages final
    {
        std::uint32_t resource_index;
        nri::StageBits stages;
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

    struct BatchRecording final
    {
        std::uint32_t first_pass, end_pass;
        bool reusable = false, output_tail = false;
        bool initialize_history = false, restore_graph = false;
    };

    struct SubmissionBatch final
    {
        EQueueRole queue = EQueueRole::GRAPHICS;
        std::uint32_t first_pass = 0;
        std::uint32_t pass_count = 0;
        std::span<const SubmissionDependency> dependencies;
        std::span<const SubmissionSuccessor> successors;
        // Imported bindings and selected History banks share the same wait and acceptance contract.
        std::span<const ResourceUseStages> resource_frontiers;
        std::span<const BatchRecording> recordings;
        bool uses_history = false, writes_output = false, accesses_output = false;
        std::span<const std::uint32_t> output_regions;
        std::span<const std::uint32_t> raster_list_passes;

        [[nodiscard]] std::uint32_t outputBegin() const noexcept
        {
            return recordings.back().output_tail ? recordings.back().first_pass : kGraphInvalidIndex;
        }

        [[nodiscard]] bool reusesPass(std::uint32_t pass) const noexcept
        {
            return recordings.front().reusable && pass < recordings.front().end_pass;
        }
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
        bool alias_eligible = false;
    };

    // Logical bindings borrow these unique physical slots; the instance owns one set per FIF.
    struct PhysicalResourceSlotIndex final
    {
        std::uint32_t index = kGraphInvalidIndex;
        [[nodiscard]] bool valid() const noexcept { return index != kGraphInvalidIndex; }
        bool operator==(const PhysicalResourceSlotIndex&) const noexcept = default;
    };

    // Each point retains the first and last occurrence of one native scope on a declared range.
    struct AliasUsePoint final
    {
        VGraphRange range;
        nri::AccessStage scope;
        bool writes;
        std::uint32_t first_pass, last_pass;
        bool needs_entry = true;
    };

    struct AliasQueueFrontier final
    {
        std::uint32_t first_point = 0, point_count = 0;
        std::uint32_t barrier_pass = kGraphInvalidIndex;
        std::uint32_t last_submission = kGraphInvalidIndex;
        // Latest whole-submit signal directly waited on at every required entry stage, per physical queue.
        std::array<std::uint32_t, 3> waited_through{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
        nri::AccessStage source{}, destination{};
    };

    struct AliasUseFrontier final
    {
        std::array<AliasQueueFrontier, 3> queues{};
        bool image = false;
        bool rendering_boundary = true;
    };

    struct PhysicalResourcePlan final
    {
        ETransientAllocation allocation = ETransientAllocation::REUSE_OBJECTS;
        std::vector<PhysicalResourceSlot> slots;
        std::vector<PhysicalResourceSlotIndex> resource_slots;
        std::vector<AliasUseFrontier> alias_frontiers;
        std::vector<AliasUsePoint> alias_points;

        [[nodiscard]] bool reusesObjects() const noexcept
        {
            for (std::size_t i = 0; i < resource_slots.size(); ++i)
            {
                const auto slot = resource_slots[i];
                if (slot.valid() && slots[slot.index].resource.index != i) return true;
            }
            return false;
        }
    };

    struct NativeProgramSlot final
    {
        nri::Pipeline* pipeline = nullptr;
        PassDescriptorBindings descriptors;
        NativeProgramId program;
        CompiledPrograms::LayoutId layout = CompiledPrograms::LayoutId::INVALID;
    };

    // Immutable native operands. The compilation result or device variant owns their use relations.
    class LUX_RENDER_PUBLIC NativeProgramBindings final
    {
    public:
        NativeProgramBindings(NativeProgramBindings&&) noexcept;
        NativeProgramBindings(const NativeProgramBindings&) = delete;
        NativeProgramBindings& operator=(NativeProgramBindings&&) = delete;
        NativeProgramBindings& operator=(const NativeProgramBindings&) = delete;
        ~NativeProgramBindings() noexcept;

        [[nodiscard]] const NativeProgramSlot& operator[](std::size_t slot) const noexcept
        {
            return slots_[slot];
        }

        [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
        [[nodiscard]] const nri::Device* device() const noexcept;
        [[nodiscard]] nri::Descriptor* sampler(std::uint32_t slot) const noexcept { return samplers_[slot]; }
        [[nodiscard]] std::span<const CompiledRasterStream> rasterStreams() const & noexcept;
        std::span<const CompiledRasterStream> rasterStreams() const && = delete;

    private:
        friend class RenderGraphCompiler;
        friend class CompiledPrograms;
        friend class compiled::RenderGraph;
        NativeProgramBindings() noexcept;
        // A failed compiler candidate has no submitted users. Shared cache entries still carry their own GPU uses.
        void discardUnsubmitted() noexcept;

        DeviceSession* owner_ = nullptr;
        std::vector<NativeProgramSlot> slots_;
        std::vector<nri::Descriptor*> samplers_; // Borrowed from this device's DescriptorTable.
        std::vector<CompiledRasterStream> streams_;
    };

} // namespace lux::render::detail

namespace lux::render::compiled
{
    struct BindingOperand final
    {
        enum class EDomain : std::uint8_t { TARGET, STORAGE };
        EDomain domain;
        std::uint32_t slot;
    };

    struct BindingComparison final
    {
        BindingOperand left, right;
    };

    // The compiler completes this device graph before its Effect publishes a const owner.
    class LUX_RENDER_PUBLIC RenderGraph final
    {
    public:
        ~RenderGraph() noexcept;
        RenderGraph(RenderGraph&& other) noexcept;
        RenderGraph& operator=(RenderGraph&&) = delete;
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;

        [[nodiscard]] const nri::Device* device() const noexcept
        {
            return programs_.device();
        }


        [[nodiscard]] std::span<const detail::CompiledResource> resources() const & noexcept
        {
            return resources_;
        }
        std::span<const detail::CompiledResource> resources() const && = delete;

        [[nodiscard]] std::span<const detail::CompiledPass> passes() const & noexcept
        {
            return recording_.passes;
        }
        std::span<const detail::CompiledPass> passes() const && = delete;

        [[nodiscard]] std::span<const detail::RecordingUnit> recordingUnits() const & noexcept
        {
            return recording_.units;
        }
        std::span<const detail::RecordingUnit> recordingUnits() const && = delete;

        [[nodiscard]] const detail::RasterCommands* rasterCommands(const detail::CompiledPass& pass) const & noexcept
        {
            return recording_.rasterCommands(pass);
        }
        const detail::RasterCommands* rasterCommands(const detail::CompiledPass&) const && = delete;

        [[nodiscard]] const detail::ComputeCommands* computeCommands(const detail::CompiledPass& pass) const & noexcept
        {
            return recording_.computeCommands(pass);
        }
        const detail::ComputeCommands* computeCommands(const detail::CompiledPass&) const && = delete;

        [[nodiscard]] const detail::TransferCommands*
        transferCommands(const detail::CompiledPass& pass) const & noexcept
        {
            return recording_.transferCommands(pass);
        }
        const detail::TransferCommands* transferCommands(const detail::CompiledPass&) const && = delete;

        [[nodiscard]] const detail::RecordingProgram& recordingProgram() const & noexcept
        {
            return recording_;
        }
        const detail::RecordingProgram& recordingProgram() const && = delete;

        [[nodiscard]] const detail::RecordingDependencies& recordingDependencies() const & noexcept;
        const detail::RecordingDependencies& recordingDependencies() const && = delete;

        [[nodiscard]] std::span<const detail::DescriptorAccess>
        descriptorUses(std::uint32_t binding_slot) const & noexcept;
        std::span<const detail::DescriptorAccess> descriptorUses(std::uint32_t) const && = delete;

        [[nodiscard]] std::span<const detail::HistoryPlan> histories() const & noexcept { return histories_; }
        std::span<const detail::HistoryPlan> histories() const && = delete;

        [[nodiscard]] std::uint32_t output() const noexcept
        {
            return output_;
        }

        [[nodiscard]] bool compatible(const GraphOutputRequirements& target) const noexcept;

        [[nodiscard]] std::span<const detail::SubmissionBatch> submissions() const & noexcept
        {
            return submissions_;
        }
        std::span<const detail::SubmissionBatch> submissions() const && = delete;


        [[nodiscard]] std::uint32_t conditionCount() const noexcept { return condition_count_; }
        [[nodiscard]] std::uint32_t bindingPredicateCount() const noexcept { return binding_predicate_count_; }
        [[nodiscard]] const detail::RasterDrawLimits& rasterDrawLimits() const & noexcept { return raster_draw_limits_; }
        const detail::RasterDrawLimits& rasterDrawLimits() const && = delete;

        [[nodiscard]] const detail::GraphBindingPlan& bindingPlan() const & noexcept { return *binding_plan_; }
        const detail::GraphBindingPlan& bindingPlan() const && = delete;

        [[nodiscard]] const detail::PhysicalResourcePlan& physicalResources() const & noexcept
        {
            return physical_resources_;
        }
        const detail::PhysicalResourcePlan& physicalResources() const && = delete;

        [[nodiscard]] const detail::NativeProgramBindings& programs() const & noexcept { return programs_; }
        const detail::NativeProgramBindings& programs() const && = delete;

        [[nodiscard]] std::span<const detail::DynamicBuffer> dynamicBuffers() const & noexcept { return dynamic_buffers_; }
        std::span<const detail::DynamicBuffer> dynamicBuffers() const && = delete;
        // Upload and CPU-sized slots are followed by live slots sized from bound resources.
        [[nodiscard]] std::size_t preparedBufferCount() const noexcept { return prepared_buffer_count_; }

        [[nodiscard]] std::span<const detail::TargetImageSlot> targetImports() const & noexcept
        {
            return target_imports_;
        }
        std::span<const detail::TargetImageSlot> targetImports() const && = delete;



        [[nodiscard]] bool writesOutputUnconditionally() const noexcept { return writes_output_unconditionally_; }
        [[nodiscard]] std::span<const std::uint32_t> conditionalOutputWrites() const & noexcept { return output_write_regions_; }
        std::span<const std::uint32_t> conditionalOutputWrites() const && = delete;

        [[nodiscard]] std::span<const BindingComparison> bindingComparisons() const & noexcept
        {
            return binding_comparisons_;
        }
        std::span<const BindingComparison> bindingComparisons() const && = delete;

    private:
        friend class detail::RenderGraphCompiler;
        friend class detail::FrameExecutor;
        friend class detail::SubmissionOperation;
        friend bool detail::dumpPlan(const detail::PreparedRenderGraph&, EGraphDumpFormat, DumpSink) noexcept;

        explicit RenderGraph(GraphOutputRequirements output) noexcept;
        std::unique_ptr<detail::GraphBindingPlan> binding_plan_;
        detail::NativeProgramBindings programs_;
        std::vector<detail::TargetImageSlot> target_imports_;
        std::vector<BindingComparison> binding_comparisons_;
        std::vector<detail::DynamicBuffer> dynamic_buffers_;
        std::size_t prepared_buffer_count_ = 0;

        detail::RasterDrawLimits raster_draw_limits_;
        bool device_addresses_ = false;
        GraphOutputRequirements output_description_;
        std::vector<detail::CompiledResource> resources_;
        std::vector<detail::HistoryPlan> histories_;
        std::uint32_t condition_count_ = 0, binding_predicate_count_ = 0;
        detail::RecordingProgram recording_;
        std::vector<detail::ResourceUseStages> batch_resource_frontiers_;
        std::vector<std::uint32_t> batch_raster_list_passes_;
        std::vector<detail::BatchRecording> batch_recordings_;
        std::vector<detail::SubmissionDependency> batch_dependencies_;
        std::vector<detail::SubmissionSuccessor> batch_successors_;
        std::vector<detail::SubmissionBatch> submissions_;
        detail::QueueBatchSlots last_batches_;
        std::vector<std::uint32_t> output_write_regions_;
        bool writes_output_unconditionally_ = false;
        detail::PhysicalResourcePlan physical_resources_;

        std::uint32_t output_ = kGraphInvalidIndex;
        std::uint32_t output_first_batch_ = kGraphInvalidIndex;
        nri::StageBits output_wait_stages_ = nri::StageBits::ALL;
    };
} // namespace lux::render::compiled

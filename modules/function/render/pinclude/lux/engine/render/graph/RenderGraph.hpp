#pragma once

#include <lux/engine/render/device/CompiledPrograms.hpp>

#include <lux/engine/core/execution/Scheduler.hpp>
#include <exec/task.hpp>
#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/device/SubmissionQueue.hpp>
#include <lux/engine/render/extensions/graph/Diagnostics.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <lux/engine/render/extensions/graph/PassValues.hpp>
#include <lux/engine/render/graph/CommandPackets.hpp>
#include <memory>

#include <lux/engine/render/effect/EffectStorageImport.hpp>
#include <lux/engine/render/effect/CpuCall.hpp>
#include <lux/engine/render/resource/RasterProgramBuild.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <lux/engine/render/graph/TargetImageSlot.hpp>

namespace lux::render::compiled { class RenderGraph; }

namespace lux::render::detail
{
    struct ConstantBufferInput final
    {
        std::uint32_t state, bytes;
        GraphResourceId resource;
        extension_detail::BoundConstantValue (*bind)(const void*) noexcept;
    };

    struct InputSlot final
    {
        std::uint32_t index;
    };

    struct CompiledPassSlot final
    {
        std::uint32_t index = kGraphInvalidIndex;
    };

    struct CpuByteRange final
    {
        std::size_t size, offset = 0;

        [[nodiscard]] std::span<std::byte> slice(std::span<std::byte> storage) const noexcept
        {
            return storage.subspan(offset, size);
        }
    };

    struct LinkedInlineBlock final
    {
        std::uint32_t pass;
        CpuByteRange bytes;
        std::uint32_t alternate = kGraphInvalidIndex;
    };

    struct PreparationRange final
    {
        std::uint32_t first = 0, count = 0;

        [[nodiscard]] bool contains(std::uint32_t slot) const noexcept
        {
            return slot >= first && slot - first < count;
        }
    };

    struct LinkedRasterStream final
    {
        std::uint32_t pass, list;
    };

    struct LinkedRasterBuild final
    {
        std::uint32_t input_owner;
        RasterProgramBuilder::Build build = nullptr;
        PreparationRange snapshots;

        [[nodiscard]] bool link(const RasterProgramBuilder& builder, const extension_detail::PreparationCall& call) noexcept
        {
            build = builder.link(call.inputs.type(), call.methods->mapping_type);
            return build != nullptr;
        }
    };

    struct CpuCall final
    {
        extension_detail::PreparationCall call;
        std::uint32_t workspace_slot = kGraphInvalidIndex;
        std::uint32_t program_slot = kGraphInvalidIndex;
        PreparationRange inputs, inline_blocks, streams, buffers;
        std::uint32_t upload_count = 0, first_snapshot = 0, first_output = 0;
    };

    struct CpuProgram final
    {
        struct Buffer final
        {
            std::uint32_t resource;
            std::uint64_t minimum_bytes, maximum_bytes;
            bool upload;
            bool used;
        };

        struct Snapshot final
        {
            cxx::basic_type_info type;
            std::uint32_t binding;
        };

        struct EffectInput final
        {
            std::uint32_t source;
            InputSlot destination;
            std::span<const std::byte> (*bind)(const void*) noexcept = nullptr;
        };

        struct ConstantCopy final
        {
            InputSlot input;
            CompiledPassSlot pass;
        };

        static_assert(std::is_standard_layout_v<ConstantCopy> && std::is_trivially_copyable_v<ConstantCopy>);

        std::vector<ConstantBufferInput> constant_buffers;
        std::vector<ConstantCopy> constant_copies;
        std::vector<EffectInput> effect_inputs;
        std::vector<CpuByteRange> cpu_values;
        std::size_t byte_count = 0;
        std::vector<InputSlot> cpu_reads;
        std::vector<LinkedInlineBlock> inline_blocks;
        std::vector<Buffer> buffers;
        // Frame-selected sources are the leading range; program-only sources are bound for cold builds.
        std::uint32_t frame_snapshot_count = 0;
        std::vector<Snapshot> snapshots;
        std::vector<std::uint32_t> snapshot_reads;
        struct DrawSourceRange final
        {
            std::uint32_t first = 0, count = 0;
            std::uint32_t producer = kGraphInvalidIndex, list = kGraphInvalidIndex;
        };

        std::vector<DrawSourceRange> draw_resource_ranges;
        struct DrawBinding final
        {
            std::uint32_t binding;
            std::uint32_t capabilities;
            bool indexed_geometry;
        };

        std::vector<DrawBinding> draw_resource_bindings;
        std::vector<LinkedRasterBuild> raster_builds;
        std::vector<std::uint32_t> program_reads;
        std::uint32_t workspace_count = 0;
        std::vector<CpuCall> calls;
        std::vector<LinkedRasterStream> streams;

        [[nodiscard]] std::span<const LinkedRasterStream> streamsFor(const CpuCall& call) const & noexcept
        {
            return std::span{streams}.subspan(call.streams.first, call.streams.count);
        }
    };

    struct GraphBindingPlan;
    struct RecordingDependencies;
    struct DescriptorAccess;
    class DeviceSession;
    class GraphCompiler;
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
        // Immutable reverse indexes shared by every Context frame slice.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> barrier_uses, descriptor_uses;
        std::vector<std::uint32_t> final_barrier_uses;
        std::vector<ImageViewBinding> image_views;
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
        BufferLocation location;
        nri::IndexType type;
    };

    struct IndexedDrawPacket final
    {
        IndexSource index;
        nri::DrawIndexedDesc draw;
    };

    struct IndirectDrawPacket final
    {
        IndexSource index;
        BufferLocation arguments, count_buffer;
        std::uint32_t count, stride;
    };

    using VRasterDraw = std::variant<nri::DrawDesc, IndexedDrawPacket, IndirectDrawPacket, RasterListCommands>;

    struct RasterCommands final
    {
        // Fixed raster program. Dynamic raster lists carry the program on each draw packet.
        std::vector<VertexSource> vertices;
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

    struct CompiledPass final
    {
        VCommandSlot commands;
        FixedBindingDelta fixed_bindings;
        std::uint32_t binding_join = kGraphInvalidIndex;
        BarrierBatch before;
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
        std::vector<ComputeCommands> compute;
        std::vector<TransferCommands> transfer;
        TransferProgram transfers;
        std::vector<RenderingGroup> groups;
        std::vector<ConditionalRegion> conditions;
        std::vector<BindingJoin> binding_joins;
        BarrierBatch after;
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
        std::vector<GraphResourceId> imported_resources;
        std::uint32_t output_begin = kGraphInvalidIndex;
        bool reusable_recording = true;
        bool writes_output = false;
        std::uint32_t first_output_region = 0, output_region_count = 0;
        std::vector<std::uint32_t> shared_draw_reads;
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
        [[nodiscard]] const CompiledRasterStream* rasterStream(std::uint32_t pass) const noexcept
        {
            return streams_[pass].get();
        }

    private:
        friend class GraphCompiler;
        friend class CompiledPrograms;
        friend class compiled::RenderGraph;
        NativeProgramBindings() noexcept;

        DeviceSession* owner_ = nullptr;
        std::vector<NativeProgramSlot> slots_;
        std::vector<nri::Descriptor*> samplers_; // Borrowed from this device's DescriptorTable.
        std::vector<std::unique_ptr<CompiledRasterStream>> streams_;
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

        [[nodiscard]] const EffectOutputDesc& outputDescription() const & noexcept
        {
            return output_description_;
        }
        const EffectOutputDesc& outputDescription() const && = delete;

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

        [[nodiscard]] bool compatible(const TargetDesc& target) const noexcept;

        [[nodiscard]] std::span<const detail::SubmissionBatch> submissions() const & noexcept
        {
            return submissions_;
        }
        std::span<const detail::SubmissionBatch> submissions() const && = delete;


        [[nodiscard]] std::uint32_t conditionCount() const noexcept { return condition_count_; }
        [[nodiscard]] std::uint32_t bindingPredicateCount() const noexcept { return binding_predicate_count_; }
        [[nodiscard]] const detail::GraphBindingPlan& bindingPlan() const & noexcept { return *binding_plan_; }
        const detail::GraphBindingPlan& bindingPlan() const && = delete;

        [[nodiscard]] const detail::PhysicalResourcePlan& physicalResources() const & noexcept
        {
            return physical_resources_;
        }
        const detail::PhysicalResourcePlan& physicalResources() const && = delete;

        [[nodiscard]] const detail::NativeProgramBindings& programs() const & noexcept { return programs_; }
        const detail::NativeProgramBindings& programs() const && = delete;

        [[nodiscard]] const detail::CpuProgram& cpuProgram() const & noexcept { return cpu_program_; }
        const detail::CpuProgram& cpuProgram() const && = delete;

        [[nodiscard]] std::span<const detail::TargetImageSlot> targetImports() const & noexcept
        {
            return target_imports_;
        }
        std::span<const detail::TargetImageSlot> targetImports() const && = delete;

        [[nodiscard]] std::span<const runtime_detail::EffectStorageImport> storageImports() const & noexcept
        {
            return storage_imports_;
        }
        std::span<const runtime_detail::EffectStorageImport> storageImports() const && = delete;

        [[nodiscard]] std::span<const detail::FinalBufferUse> outputUses() const & noexcept { return output_uses_; }
        std::span<const detail::FinalBufferUse> outputUses() const && = delete;

        [[nodiscard]] std::span<const detail::FinalBufferExport> bufferExports() const & noexcept
        {
            return buffer_exports_;
        }
        std::span<const detail::FinalBufferExport> bufferExports() const && = delete;

        [[nodiscard]] std::span<const BindingComparison> bindingComparisons() const & noexcept
        {
            return binding_comparisons_;
        }
        std::span<const BindingComparison> bindingComparisons() const && = delete;

    private:
        friend class detail::GraphCompiler;
        friend class detail::FrameExecutor;
        friend class detail::SubmissionOperation;
        friend bool detail::dumpPlan(const detail::PreparedRenderGraph&, EGraphDumpFormat, DumpSink) noexcept;

        explicit RenderGraph(EffectOutputDesc output) noexcept;
        std::unique_ptr<detail::GraphBindingPlan> binding_plan_;
        detail::NativeProgramBindings programs_;
        std::vector<detail::TargetImageSlot> target_imports_;
        std::vector<runtime_detail::EffectStorageImport> storage_imports_;
        std::vector<detail::FinalBufferUse> output_uses_;
        std::vector<detail::FinalBufferExport> buffer_exports_;
        std::vector<BindingComparison> binding_comparisons_;
        detail::CpuProgram cpu_program_;

        bool device_addresses_ = false;
        EffectOutputDesc output_description_;
        std::vector<detail::CompiledResource> resources_;
        std::vector<detail::HistoryPlan> histories_;
        std::uint32_t condition_count_ = 0, binding_predicate_count_ = 0;
        detail::RecordingProgram recording_;
        std::vector<detail::SubmissionBatch> submissions_;
        detail::QueueBatchSlots first_batches_, last_batches_, last_output_batches_;
        std::vector<std::uint32_t> output_write_regions_;
        detail::PhysicalResourcePlan physical_resources_;

        std::uint32_t output_ = kGraphInvalidIndex;
        std::uint32_t output_first_batch_ = kGraphInvalidIndex;
        nri::StageBits output_wait_stages_ = nri::StageBits::ALL;
        std::array<std::uint32_t, 2> fixed_output_extent_{};
    };
} // namespace lux::render::compiled

namespace lux::render::detail
{
    struct GraphDescriptorDiagnostics final
    {
        std::string name;
        std::uint32_t set, binding, count, stages, record_bytes;
        std::uint8_t kind;
        bool runtime_records;
    };

    struct GraphCompileDiagnostics final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC std::uint32_t passSlot(GraphPassId) const noexcept;
        std::vector<GraphPassId> source_passes;
        std::vector<std::string> resource_keys;
        std::vector<EResourceReuse> resource_reuse;
        std::vector<std::vector<GraphDescriptorDiagnostics>> descriptor_fields;
    };

    struct LUX_RENDER_PUBLIC PreparedRenderGraph final
    {
        compiled::RenderGraph graph;
        GraphCompileDiagnostics diagnostics;
    };
} // namespace lux::render::detail

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
#include <lux/engine/render/graph/GraphAssemblyPlan.hpp>
#include <lux/engine/render/graph/CommandPackets.hpp>
#include <lux/engine/render/graph/execute/PassRecordInput.hpp>
#include <memory>

namespace lux::render::detail
{
    struct GraphBindingPlan;
    struct DescriptorAccess;
    class DeviceSession;
    class RenderGraphExecutable;
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
        [[nodiscard]] RenderResult<void>
        record(const PassRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&) const noexcept;

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
        std::array<float, 2> depth_bias{};
        bool section_depth_bias = false;
    };

    struct CompiledPass final
    {
        VCommandSlot commands;
        bool writes_output = false;
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
        std::vector<RenderingGroup> groups;
        std::vector<ConditionalRegion> conditions;
        BarrierBatch after;
    };

    template <class Recorder>
    [[nodiscard]] RenderResult<void>
    recordBlock(const RecordingProgram& program, VCommandSlot block, Recorder recorder) noexcept
    {
        return std::visit([&](auto slot) noexcept -> RenderResult<void>
        {
            if constexpr (slot.kind == EGraphPassKind::GRAPHICS) return recorder(program.raster[slot.index]);
            else if constexpr (slot.kind == EGraphPassKind::COMPUTE) return recorder(program.compute[slot.index]);
            else return recorder(program.transfer[slot.index]);
        }, block);
    }

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
        std::vector<std::uint32_t> shared_draw_reads;
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
        bool alias_eligible = false;
    };

    // Logical bindings borrow these unique physical slots; the instance owns one set per FIF.
    struct PhysicalResourceSlotIndex final
    {
        std::uint32_t index = kGraphInvalidIndex;
        [[nodiscard]] bool valid() const noexcept { return index != kGraphInvalidIndex; }
        bool operator==(const PhysicalResourceSlotIndex&) const noexcept = default;
    };

    struct PhysicalResourcePlan final
    {
        ETransientAllocation allocation = ETransientAllocation::REUSE_OBJECTS;
        std::vector<PhysicalResourceSlot> slots;
        std::vector<PhysicalResourceSlotIndex> resource_slots;
    };

    // The execution candidate owns this immutable result; its graph instance borrows it until teardown.
    class LUX_RENDER_PUBLIC RenderGraphExecutable final
    {
    public:
        ~RenderGraphExecutable() noexcept;
        RenderGraphExecutable(RenderGraphExecutable&& other) noexcept;
        RenderGraphExecutable& operator=(RenderGraphExecutable&&) = delete;
        RenderGraphExecutable(const RenderGraphExecutable&) = delete;
        RenderGraphExecutable& operator=(const RenderGraphExecutable&) = delete;

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

        [[nodiscard]] std::span<const RecordingUnit> recordingUnits() const noexcept
        {
            return recording_.units;
        }

        [[nodiscard]] const RasterCommands* rasterCommands(const CompiledPass& pass) const noexcept
        {
            return recording_.rasterCommands(pass);
        }

        [[nodiscard]] const ComputeCommands* computeCommands(const CompiledPass& pass) const noexcept
        {
            return recording_.computeCommands(pass);
        }

        [[nodiscard]] const TransferCommands* transferCommands(const CompiledPass& pass) const noexcept
        {
            return recording_.transferCommands(pass);
        }

        [[nodiscard]] const RecordingProgram& recordingProgram() const noexcept
        {
            return recording_;
        }

        [[nodiscard]] std::span<const DescriptorAccess> descriptorUses(std::uint32_t binding_slot) const noexcept;

        [[nodiscard]] std::uint32_t output() const noexcept
        {
            return output_;
        }

        [[nodiscard]] bool compatible(const TargetDesc& target) const noexcept;

        [[nodiscard]] std::span<const SubmissionBatch> submissions() const noexcept
        {
            return submissions_;
        }

        [[nodiscard]] std::span<const ImportedAccess> importedAccesses() const noexcept { return imported_accesses_; }

        [[nodiscard]] const PhysicalResourcePlan& physicalResources() const noexcept
        {
            return physical_resources_;
        }

    private:
        friend class RenderGraphInstance;
        friend class GraphCompiler;
        friend class FrameExecutor;
        friend class SubmissionOperation;
        friend LUX_RENDER_PUBLIC bool dumpPlan(const PreparedRenderGraph&, EGraphDumpFormat, DumpSink) noexcept;

        RenderGraphExecutable(nri::Device* device, EffectOutputDesc output) noexcept;
        std::unique_ptr<GraphBindingPlan> binding_plan_;
        nri::Device* device_;
        bool device_addresses_ = false;
        EffectOutputDesc output_description_;
        std::vector<CompiledResource> resources_;
        std::vector<HistoryPlan> histories_;
        std::uint32_t condition_count_ = 0;
        RecordingProgram recording_;
        std::vector<SubmissionBatch> submissions_;
        std::vector<ImportedAccess> imported_accesses_;
        PhysicalResourcePlan physical_resources_;

        std::uint32_t output_ = kGraphInvalidIndex;
        std::uint32_t output_first_batch_ = kGraphInvalidIndex;
        nri::StageBits output_wait_stages_ = nri::StageBits::ALL;
        std::array<std::uint32_t, 2> fixed_output_extent_{};
    };

    struct NativeProgramSlot final
    {
        nri::Pipeline* pipeline = nullptr;
        PassDescriptorBindings descriptors;
        CompiledPrograms::ProgramId program;
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
        [[nodiscard]] const nri::Device* device() const noexcept { return device_; }
        [[nodiscard]] const CompiledRasterStream* rasterStream(std::uint32_t pass) const noexcept
        {
            return streams_[pass].get();
        }

    private:
        friend class GraphCompiler;
        friend class CompiledPrograms;
        friend struct PreparedRenderGraph;
        NativeProgramBindings(nri::Device& device, std::size_t slots) noexcept;

        const nri::Device* device_;
        std::vector<NativeProgramSlot> slots_;
        std::vector<std::unique_ptr<CompiledRasterStream>> streams_;
    };

    struct GraphDescriptorDiagnostics final
    {
        std::string name;
        std::uint32_t set, binding, count, stages, record_bytes;
        std::uint8_t kind;
        bool runtime_records;
    };

    struct GraphCompileDiagnostics final
    {
        std::vector<GraphPassId> source_passes;
        std::vector<std::string> resource_keys;
        std::vector<EResourceReuse> resource_reuse;
        std::vector<std::vector<GraphDescriptorDiagnostics>> descriptor_fields;
    };

    struct LUX_RENDER_PUBLIC PreparedRenderGraph final
    {
        PreparedRenderGraph(PreparedRenderGraph&&) noexcept;
        ~PreparedRenderGraph() noexcept;
        [[nodiscard]] NativeProgramBindings takePrograms() && noexcept;

        RenderGraphExecutable executable;
        NativeProgramBindings programs;
        GraphAssemblyPlan assembly;
        GraphCompileDiagnostics diagnostics;

    private:
        friend class GraphCompiler;
        PreparedRenderGraph(DeviceSession&, RenderGraphExecutable) noexcept;
        DeviceSession* owner_;
    };
} // namespace lux::render::detail

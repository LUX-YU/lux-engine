#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/target/TargetOutput.hpp>
#include <lux/engine/render/extensions/execution/PreparedCpuValue.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/graph/PreparationPlan.hpp>
#include <lux/engine/render/graph/execute/OwnedRasterStream.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::runtime_detail { struct JobExecution; }

namespace lux::render::detail
{
    struct FrozenPatchCounts;
    struct FrozenOperandPatches;
    struct FrameResourceBinding;
    struct FrameResources;
    struct HistoryState;
    class DeviceSession;
    struct RasterDrawList;
    struct RasterDescriptorLayout;
    class JobGpuResources;
    struct RasterBindingKey;
    struct RasterDescriptorInput;
    struct FrozenRecordingFrame;
    struct DynamicRasterPrograms;
    struct RasterProgramUpdate;

    class LUX_RENDER_PUBLIC RenderGraphInstance final
    {
    public:
        [[nodiscard]] std::uint32_t frameCount() const noexcept;
        struct SnapshotSource final
        {
            ResourceInstanceId id;
            void* storage;
            const StorageSnapshotOps* ops;
        };

        struct PreparationSources final
        {
            std::vector<BoundConstantBuffer> constant_buffers;
            std::vector<SnapshotSource> snapshots;
            std::vector<BoundDrawResource> draw_resources;
            std::vector<std::span<const std::byte>> effect_values;
        };

        [[nodiscard]] RenderResult<void>
        createProducers(const PreparationPlan&, const ResourceRegistry&, PreparationSources) noexcept;
        [[nodiscard]] RenderResult<void> prepareProducers(std::uint32_t frame_slot) noexcept;
        void installPreparation(RenderJobId job, std::uint64_t generation) noexcept;

        [[nodiscard]] RenderResult<bool> stagePreparedBuffers(
            std::uint32_t frame, ResourceUploader*, std::vector<PreparedBufferCopy>&
        ) noexcept;
        void settlePreparedBuffer(std::uint32_t, const PreparedBufferCopy&, SubmissionPoint, RenderError) noexcept;
        [[nodiscard]] bool hasExclusiveProgramInput() const noexcept;
        [[nodiscard]] std::uint64_t programInputRevision() const noexcept { return program_input_revision_; }
        [[nodiscard]] bool needsProgramBuild() const noexcept
        {
            return applied_program_revision_ != program_input_revision_;
        }
        [[nodiscard]] RenderResult<void> selectProgramUpdate(RasterProgramUpdate&) noexcept;
        void adoptProgramUpdate(RasterProgramUpdate&) noexcept;

        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] RenderResult<void>
        writeInlineConstant(
            std::uint32_t slot, std::uint32_t pass, std::span<const std::byte> bytes, std::uint64_t revision = 0
        ) noexcept;
        // Caller holds the writable FIF until recording and all accepted GPU uses are complete.
        // Each batch part has an exclusive allocator; encoders only borrow its prepared data.
        [[nodiscard]] RenderResult<std::span<nri::CommandBuffer* const>>
        recordBatch(std::uint32_t frame_slot, std::uint32_t batch) noexcept;


        struct ResizeResources;

        struct ImageReplacement final
        {
            GraphResourceId resource;
            std::span<const TargetFrame> frames;
        };

        // Cold candidate only; instance identity, subscriptions and Feature bindings remain unchanged.
        // Empty output is valid only for graphs with no output or Target-relative resources.
        // Surface withdrawal uses releaseOutputReferences(), not an empty Resize candidate.
        // Revalidate dependent extents after suspension; concurrent Storage publication may change buffer ranges.
        [[nodiscard]] RenderResult<void> prepareResizeAdoption(ResizeResources&) noexcept;
        // Called only after affected accepted GPU uses complete, with no intervening suspension.
        void commitResize(ResizeResources&&) noexcept;
        // After accepted uses complete, withdraw borrowed SwapChain objects before their presentation owner closes.
        void releaseOutputReferences() noexcept;
        // The selected writable FIF slot owns the list. Features fill physical packets before
        // prepareFrame seals attachments and barriers; no list is mutated during recording.
        [[nodiscard]] RasterDrawList&
        rasterDraws(const CompiledPass&, std::uint32_t slot, bool recording_changed = true) noexcept;
        // Prepare one owned dynamic array in the already-acquired FIF slot. Views are created
        // before publication; failure preserves the prior image and no CPU/GPU wait is introduced.
        [[nodiscard]] RenderResult<std::span<nri::Descriptor* const>>
        prepareImageLayers(GraphResourceId, std::uint32_t slot, std::uint32_t layers) noexcept;
        // Borrowed physical inputs are installed before initialize; the caller retains them through GPU completion.
        void bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept;

        [[nodiscard]] RenderResult<void>
        qualifyImportedImage(GraphResourceId, const nri::Texture&, const TargetDesc&) const noexcept;

        [[nodiscard]] bool frozenForSubmission(std::uint32_t slot) const noexcept { return frozen_slot_ == slot; }

        ~RenderGraphInstance() noexcept;
        RenderGraphInstance(const RenderGraphInstance&) = delete;
        RenderGraphInstance& operator=(const RenderGraphInstance&) = delete;

        [[nodiscard]] const compiled::RenderGraph& plan() const & noexcept
        {
            return graph_;
        }
        const compiled::RenderGraph& plan() const && = delete;

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return output_.description;
        }

        [[nodiscard]] RenderResult<void> bindPredicates(std::vector<std::uint8_t>) noexcept;
        [[nodiscard]] RenderResult<void> setConditions(std::span<const std::uint8_t>) noexcept;

        [[nodiscard]] std::span<const std::uint8_t> conditions() const noexcept
        {
            return condition_values_;
        }

        // An empty selection invalidates every declared history at the next preparation boundary.
        [[nodiscard]] RenderResult<void> invalidateHistory(std::span<const GraphResourceId> previous = {}) noexcept;

        void setViewport(const RasterViewport& viewport) noexcept
        {
            viewport_ = viewport;
        }

        [[nodiscard]] const RasterViewport& viewport() const noexcept
        {
            return viewport_;
        }

        // FrameProgress selects the reusable slot. The round joins recording before submission.
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t slot) noexcept;

        [[nodiscard]] const ExecutionProgress& progress() const noexcept
        {
            return progress_;
        }

        // Trusted compiled resource index and Session slot, not a business resource lookup.
        void setResource(std::uint32_t resource, std::uint32_t slot, const ResourceInput&) noexcept;
        [[nodiscard]] bool frozenCondition(std::uint32_t slot, PassCondition) const noexcept;
        [[nodiscard]] ResourceInput resourceInput(GraphResourceId, std::uint32_t slot) const noexcept;
        // Cold diagnostics: sum actual NRI allocation requirements for this instance's owned resources.
        [[nodiscard]] bool dumpResources(DumpSink) const noexcept;
        [[nodiscard]] nri::Buffer* nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept;

        // Explicit dirty notification for producers that opted out of rasterDraws' conservative default.
        void invalidateRasterDraws(const CompiledPass&, std::uint32_t frame_slot) noexcept;

        [[nodiscard]] FrozenOperandPatches lastOperandPatches(std::uint32_t slot) const noexcept;
        [[nodiscard]] FrozenPatchCounts lastFrozenPatches(std::uint32_t slot) const noexcept;

    private:
        friend struct runtime_detail::JobExecution;

        struct FrameStorage final
        {
            std::span<FrameResources> frames;
            std::span<HistoryState> histories;
        };

        RenderGraphInstance(JobGpuResources&, const compiled::RenderGraph&, FrameStorage, TargetOutput) noexcept;

        friend class FrameOperation;
        std::uint32_t frozen_slot_ = kGraphInvalidIndex;
        friend class SubmissionOperation;
        [[nodiscard]] RenderResult<FrozenRecordingFrame> recordingFrame(std::uint32_t frame_slot) noexcept;
        void submitted(std::uint32_t slot) noexcept;
        void acceptedBatch(std::uint32_t slot, std::uint32_t batch, SubmissionPoint point) noexcept;
        void failedSubmission(RenderError error) noexcept;
        [[nodiscard]] std::uint32_t recordedPassCount(std::uint32_t slot) const noexcept;
        [[nodiscard]] RenderResult<void> growRasterDescriptorPool(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> createFrameResources(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareFrameBindings(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        [[nodiscard]] RenderResult<void> rebind(std::uint32_t slot) noexcept;

        [[nodiscard]] RenderResult<void> prepareResizeHistories(ResizeResources&, const TargetDesc&) noexcept;
        [[nodiscard]] RenderResult<void>
        prepareResizePhysical(ResizeResources&, std::uint32_t, const TargetDesc&) noexcept;
        [[nodiscard]] RenderResult<void> prepareResizeBindings(ResizeResources&, std::uint32_t) noexcept;

        [[nodiscard]] RenderResult<void> prepareRasterDescriptorCapacity(std::uint32_t slot, bool resize_pool) noexcept;
        [[nodiscard]] RenderResult<nri::DescriptorSet*> prepareRasterDescriptorSet(
            std::uint32_t slot, const RasterDescriptorLayout&, std::uint32_t record,
            std::span<const RasterDescriptorInput>
        ) noexcept;
        [[nodiscard]] const RasterDescriptorLayout& rasterBindings(RasterBindingKey) const noexcept;
        void releaseRasterBindings(std::uint32_t pass, std::uint64_t generation) noexcept;
        static void
        collectRasterViews(FrameResources&, nri::Buffer*, nri::Texture*, std::vector<nri::Descriptor*>&) noexcept;
        void retireRasterViews(FrameResources&, nri::Buffer*, nri::Texture*) noexcept;
        [[nodiscard]] RenderResult<nri::BufferDesc> bufferDescription(const CompiledResource&) const noexcept;
        [[nodiscard]] RenderResult<nri::BufferDesc>
        bufferDescription(const CompiledResource&, std::uint64_t bytes) const noexcept;
        [[nodiscard]] nri::Result createBufferView(nri::BufferViewDesc, nri::Descriptor*&) const noexcept;
        [[nodiscard]] RenderResult<nri::TextureDesc> textureDescription(const CompiledResource&) const noexcept;
        [[nodiscard]] RenderResult<nri::TextureDesc>
        textureDescription(const CompiledResource&, const TargetDesc&) const noexcept;
        void initializeRasterStreams(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> materializeRasterStreams(std::uint32_t slot) noexcept;
        RenderResult<void> prepareRasterSections(std::uint32_t slot) noexcept;
        RenderResult<void>
        materializeRasterSections(std::uint32_t slot, std::uint32_t pass, const OwnedRasterStream&) noexcept;
        [[nodiscard]] RenderResult<void> allocatePhysicalResources(FrameResources&) noexcept;
        void prepareAliasBarriers(FrameResources&) noexcept;
        [[nodiscard]] RenderResult<void> createResource(std::uint32_t resource, FrameResourceBinding&) noexcept;
        [[nodiscard]] RenderResult<void>
        createResource(std::uint32_t resource, FrameResourceBinding&, const TargetDesc&) noexcept;
        [[nodiscard]] RenderResult<void> createHistories() noexcept;
        [[nodiscard]] RenderResult<void> createHistoryViews(HistoryState&) noexcept;
        [[nodiscard]] bool selectHistory() noexcept;
        [[nodiscard]] bool conditionEnabled(const FrameResources&, PassCondition) const noexcept;
        void publishHistory(std::uint32_t slot) noexcept;

        DeviceSession* session_;
        JobGpuResources& gpu_;

        class ProducerFrame;
        [[nodiscard]] RenderResult<void>
        buildRasterPrograms(std::uint32_t, RasterProgramUpdate&) noexcept;
        void releaseDynamicRasterPrograms(std::uint32_t pass, bool replacing) noexcept;
        struct PreparedBytes final
        {
            std::vector<std::byte> bytes;
            bool written = false;
        };

        struct PreparedInlineBlock final
        {
            PreparedBytes payload;
            std::uint64_t source_revision = 0;
        };

        struct Producer final
        {
            std::vector<PreparedInlineBlock> constants;
            std::vector<OwnedRasterStream> streams;
            std::vector<std::uint32_t> runtime_streams{};
            std::vector<std::uint32_t> program_sources;
            RasterProgramBuilder::Mapping draw_mapping{nullptr, nullptr};
            bool programs_dirty = false;
        };

        struct PreparedBufferInput final
        {
            enum class ESource { MISSING, SIZE_ONLY, RESIDENT, OWNED_BYTES };

            ESource source = ESource::MISSING;
            std::vector<std::byte> bytes;
            std::uint64_t revision = 0, size = 0, resident_generation = 0;
        };

        const PreparationPlan* preparation_plan_ = nullptr;
        struct StorageSnapshot final
        {
            SnapshotSource source;
            StorageSnapshotOps::Selection value;
            std::uint64_t revision = 0;
            ProgramInputRevision* program_input = nullptr;
        };

        const ResourceRegistry* snapshot_registry_ = nullptr;
        std::vector<std::span<const std::byte>> effect_values_;
        std::vector<BoundDrawResource> draw_resources_;
        std::vector<StorageSnapshot> storage_snapshots_;
        std::vector<std::uint32_t> snapshot_sources_;
        std::vector<extension_detail::AlgorithmWorkspace> workspaces_;
        std::vector<Producer> producers_;
        std::vector<std::uint32_t> program_producers_;
        std::vector<ProgramInputRevision::Use> program_input_uses_;
        std::uint64_t program_input_revision_ = 0, applied_program_revision_ = 0;
        std::vector<std::unique_ptr<DynamicRasterPrograms>> dynamic_raster_programs_;
        std::vector<PreparedBytes> cpu_data_;
        std::vector<PreparedBufferInput> prepared_buffer_inputs_;
        RenderJobId preparation_job_;
        std::uint64_t preparation_generation_ = 0;
        std::uint64_t preparation_revision_ = 0;
        std::vector<BoundConstantBuffer> constant_buffers_;
        const std::span<FrameResources> frames_;
        const std::span<HistoryState> histories_;
        const compiled::RenderGraph& graph_;
        RasterViewport viewport_;
        TargetOutput output_;

        std::vector<std::uint8_t> condition_values_, binding_values_;
        ExecutionProgress progress_;
        RenderError submission_error_;
    };
} // namespace lux::render::detail

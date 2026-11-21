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
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/resources/view/Parameters.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    struct RasterDrawList;
    struct PassBindings;
    struct RasterDescriptorInput;
    struct LinkedUniformBlock;
    struct DynamicRasterPrograms;
    class SampledTexture2DStorage;
    class IndexedTriangleMeshStorage;
    class RasterMaterialStorage;

    class LUX_RENDER_PUBLIC RenderGraphInstance final
    {
    public:
        RenderGraphInstance(
            DeviceSession&,
            std::uint32_t frame_count,
            const RenderGraphExecutable&,
            const NativeProgramBindings&,
            TargetOutput = {}
        ) noexcept;
        [[nodiscard]] std::uint32_t frameCount() const noexcept;
        struct SnapshotSource final
        {
            ResourceInstanceId id;
            const void* storage;
            const StorageSnapshotOps* ops;
        };

        struct RasterOwners final
        {
            const SampledTexture2DStorage* textures = nullptr;
            const IndexedTriangleMeshStorage* meshes = nullptr;
            RasterMaterialStorage* materials = nullptr;
        };

        [[nodiscard]] RenderResult<void>
        createProducers(
            const PreparationPlan&, const ResourceRegistry&, std::span<const SnapshotSource>,
            std::vector<RasterOwners>
        ) noexcept;
        [[nodiscard]] RenderResult<void> prepareProducers(std::uint32_t frame_slot) noexcept;
        void bindPreparationOwner(RenderJobId job, std::uint64_t generation) noexcept
        {
            preparation_job_ = job;
            preparation_generation_ = generation;
        }

        [[nodiscard]] RenderResult<bool> stagePreparedBuffers(
            std::uint32_t frame, ResourceUploader*, std::vector<PreparedBufferCopy>&
        ) noexcept;
        void settlePreparedBuffer(std::uint32_t, const PreparedBufferCopy&, SubmissionPoint, RenderError) noexcept;
        void selectViewParameters(const ViewGpuParameters& value, std::uint64_t revision) noexcept
        {
            view_parameters_ = {value, revision};
        }
        [[nodiscard]] bool requiresProducerPrograms() const noexcept;
        void captureProgramRevisions(std::vector<std::uint64_t>&) const noexcept;
        [[nodiscard]] bool matchesProgramRevisions(std::span<const std::uint64_t>) const noexcept;
        [[nodiscard]] exec::task<RenderResult<bool>>
        prepareProducerPrograms(
            execution::CpuScheduler, std::span<const std::uint64_t>, stdexec::inplace_stop_token
        ) noexcept;

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
        [[nodiscard]] RenderResult<ResizeResources>
        prepareResize(TargetOutput, std::span<const ImageReplacement> inputs = {}) noexcept;
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
        bindSampler(std::uint32_t input, nri::Descriptor&, bool comparison = false) noexcept;

        [[nodiscard]] std::size_t samplerInputCount() const noexcept
        {
            return samplers_.size();
        }

        [[nodiscard]] bool usesSampler(std::uint32_t input) const noexcept
        {
            return samplers_[input].used;
        }

        ~RenderGraphInstance() noexcept;
        RenderGraphInstance(const RenderGraphInstance&) = delete;
        RenderGraphInstance& operator=(const RenderGraphInstance&) = delete;

        [[nodiscard]] nri::Device* device() const noexcept
        {
            return plan_->device();
        }

        [[nodiscard]] const RenderGraphExecutable& plan() const noexcept
        {
            return *plan_;
        }

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return output_.description;
        }

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
        void linkUniformBlocks(std::span<const LinkedUniformBlock>) noexcept;
        void setResource(std::uint32_t resource, std::uint32_t slot, const ResourceInput&) noexcept;
        [[nodiscard]] ResourceInput resourceInput(GraphResourceId, std::uint32_t slot) const noexcept;
        // Cold diagnostics: sum actual NRI allocation requirements for this instance's owned resources.
        [[nodiscard]] bool dumpResources(DumpSink) const noexcept;
        [[nodiscard]] nri::Buffer* nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept;

        // Explicit dirty notification for producers that opted out of rasterDraws' conservative default.
        void invalidateRasterDraws(const CompiledPass&, std::uint32_t frame_slot) noexcept;

    private:
        friend class FrameOperation;
        friend class SubmissionOperation;
        [[nodiscard]] RenderResult<std::span<nri::CommandBuffer* const>>
        recordFrozenBatch(std::uint32_t, std::uint32_t) noexcept;
        void submitted(std::uint32_t slot) noexcept;
        void acceptedBatch(std::uint32_t batch, SubmissionPoint point) noexcept;
        void failedSubmission(RenderError error) noexcept;
        [[nodiscard]] std::uint32_t recordedPassCount(std::uint32_t slot) const noexcept;
        [[nodiscard]] RenderResult<void> growRasterDescriptorPool(std::uint32_t slot) noexcept;
        void invalidateRecording(std::uint32_t frame_slot, std::uint32_t compiled_pass) noexcept;
        void invalidateRecordings(std::uint32_t frame_slot) noexcept;
        [[nodiscard]] RenderResult<void> createRecordings(std::uint32_t frame_slot) noexcept;
        [[nodiscard]] RenderResult<void> createFrameResources(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareFrameBindings(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        [[nodiscard]] RenderResult<void> rebind(std::uint32_t slot) noexcept;

        [[nodiscard]] RenderResult<void> prepareResizeHistories(ResizeResources&, const TargetDesc&) noexcept;
        [[nodiscard]] RenderResult<void>
        prepareResizePhysical(ResizeResources&, std::uint32_t, const TargetDesc&) noexcept;
        [[nodiscard]] RenderResult<void> prepareResizeBindings(ResizeResources&, std::uint32_t) noexcept;

        struct Binding final
        {
            nri::Texture* image = nullptr;
            nri::Descriptor* view = nullptr;
            nri::Buffer* buffer = nullptr;
            std::uint64_t buffer_offset = 0, buffer_size = 0;
            std::span<nri::Buffer* const> addressed_buffers;
            std::uint64_t addressed_revision = 0;
            bool dirty = false;
        };

        struct HistoryState;
        struct FrameResources;

        struct RasterBindingKey final
        {
            std::uint32_t pass, program;
            std::uint64_t generation;

            bool operator==(const RasterBindingKey&) const noexcept = default;
        };

        [[nodiscard]] RenderResult<nri::DescriptorSet*> prepareRasterDescriptorSet(
            std::uint32_t, const PassBindings&, RasterBindingKey, std::uint32_t draw,
            std::span<const RasterDescriptorInput>
        ) noexcept;
        [[nodiscard]] const PassBindings& rasterBindings(RasterBindingKey) const noexcept;
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
        void prepareStreamedDraws(std::uint32_t slot) noexcept;
        void freezeRecordInput(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> allocatePhysicalResources(FrameResources&) noexcept;
        void prepareAliasBarriers(FrameResources&) noexcept;
        [[nodiscard]] RenderResult<void> createResource(std::uint32_t resource, Binding&) noexcept;
        [[nodiscard]] RenderResult<void> createResource(std::uint32_t resource, Binding&, const TargetDesc&) noexcept;
        [[nodiscard]] RenderResult<void> createHistories() noexcept;
        [[nodiscard]] RenderResult<void> createHistoryViews(HistoryState&) noexcept;
        [[nodiscard]] bool selectHistory() noexcept;
        void bindHistory(std::uint32_t slot) noexcept;
        void updateHistoryViews(std::uint32_t slot) noexcept;
        void publishHistory(std::uint32_t slot) noexcept;

        struct Pass final
        {
            const CompiledPass* compiled_pass;
            std::vector<nri::TextureBarrierDesc> images;
            std::vector<nri::BufferBarrierDesc> buffers;
            std::optional<nri::GlobalBarrierDesc> alias_barrier;
            nri::DescriptorSet* private_descriptors = nullptr;
            std::vector<nri::Descriptor*> private_views;
            std::vector<nri::Descriptor*> history_views;
            std::vector<nri::AttachmentDesc> colors;
            nri::AttachmentDesc depth{};
            std::vector<nri::TextureDataLayoutDesc> copy_layouts;
            std::vector<nri::Descriptor*> clear_views;
            std::array<std::uint32_t, 3> dispatch_groups{};
            std::span<std::byte> parameters;
            std::uint64_t parameter_source_revision = 0;
        };

        DeviceSession* session_;
        SubmissionFrontiers uses_;

        class ProducerFrame;
        class ProgramSelection;
        [[nodiscard]] exec::task<RenderResult<bool>> prepareRasterPrograms(
            std::uint32_t, execution::CpuScheduler, std::span<const std::uint64_t>, stdexec::inplace_stop_token
        ) noexcept;
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
            std::unique_ptr<::lux::render::InstanceData> value;
            std::vector<PreparedInlineBlock> constants;
            std::vector<OwnedRasterStream> streams;
            std::vector<std::byte> node_state;
            std::vector<std::uint32_t> runtime_streams{};
            std::span<const std::uint32_t> program_snapshots{};
            std::vector<std::uint64_t> program_revisions{};
        };

        [[nodiscard]] bool
        rasterProgramMatches(const Producer& producer, std::span<const std::uint64_t> revisions) const noexcept;
        [[nodiscard]] std::uint64_t programStorageRevision(std::uint32_t slot) const noexcept;
        [[nodiscard]] RenderResult<extension_detail::SelectedStorageSnapshot>
        readStorageSnapshot(std::span<const std::uint32_t>, std::uint32_t, cxx::basic_type_info) const noexcept;

        struct PreparedBufferInput final
        {
            PreparedBytes payload;
            std::uint64_t revision = 0, size = 0;
        };

        const PreparationPlan* preparation_plan_ = nullptr;
        struct StorageSnapshot final
        {
            SnapshotSource source;
            StorageSnapshotOps::Selection value;
            std::uint64_t revision = 0;
        };

        const ResourceRegistry* snapshot_registry_ = nullptr;
        std::vector<RasterOwners> raster_owners_;
        std::vector<StorageSnapshot> storage_snapshots_;
        std::vector<Producer> producers_;
        std::vector<std::uint32_t> program_producers_;
        std::vector<std::unique_ptr<DynamicRasterPrograms>> dynamic_raster_programs_;
        std::vector<PreparedBytes> cpu_data_;
        std::vector<PreparedBufferInput> prepared_buffer_inputs_;
        RenderJobId preparation_job_;
        std::uint64_t preparation_generation_ = 0;
        std::uint64_t preparation_revision_ = 0;
        std::span<const LinkedUniformBlock> uniform_blocks_;
        PreparedCpuValue<ViewGpuParameters> view_parameters_{};
        std::vector<FrameResources> frames_;
        std::vector<HistoryState> histories_;
        const RenderGraphExecutable* plan_;
        const NativeProgramBindings* programs_;
        RasterViewport viewport_;
        TargetOutput output_;

        struct SamplerInput final
        {
            nri::Descriptor* descriptor = nullptr;
            bool comparison = false;
            bool used = false;
        };

        std::vector<SamplerInput> samplers_;
        std::vector<std::uint8_t> condition_values_;
        ExecutionProgress progress_;
        RenderError submission_error_;
    };
} // namespace lux::render::detail

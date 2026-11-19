#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/target/TargetOutput.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/extensions/graph/InstanceData.hpp>
#include <lux/engine/render/extensions/graph/ParameterSource.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    struct DrawListResources;
    struct RasterDrawList;
    struct PassBindings;

    class LUX_RENDER_PUBLIC RenderGraphInstance final : public InstanceAccess
    {
    public:
        RenderGraphInstance(
            DeviceSession&,
            std::uint32_t frame_count,
            const RenderGraphExecutable&,
            const NativeGraphPrograms&,
            TargetOutput = {}
        ) noexcept;
        [[nodiscard]] std::uint32_t frameCount() const noexcept;
        [[nodiscard]] RenderResult<void>
        createData(
            const GraphAssemblyPlan&, std::span<const InstanceDataRecipe>, ResourceRegistry&,
            std::span<const ResourceInstanceId>
        ) noexcept;
        [[nodiscard]] RenderResult<bool> prepareData(std::uint32_t, ResourceUploader&, Preparation&) noexcept;
        [[nodiscard]] bool requiresDataPrograms() const noexcept;
        [[nodiscard]] RenderResult<void>
        setIndexedDrawData(IndexedDrawStreamSlot, std::uint32_t, const IndexedDrawData&) noexcept override;
        [[nodiscard]] std::vector<TargetId> sampledTargets() const noexcept;
        [[nodiscard]] RenderResult<IndexedDrawStreamSlot> indexedDrawStream(GraphPassId) const noexcept override;
        [[nodiscard]] bool hasData() const noexcept { return !data_.empty(); }
        void captureProgramRevisions(std::vector<std::uint64_t>&) const noexcept;
        [[nodiscard]] bool matchesProgramRevisions(std::span<const std::uint64_t>) const noexcept;
        [[nodiscard]] exec::task<RenderResult<bool>>
        prepareDataPrograms(
            execution::CpuScheduler, std::span<const std::uint64_t>, stdexec::inplace_stop_token
        ) noexcept;

        [[nodiscard]] RenderResult<void> initialize(const GraphAssemblyPlan&) noexcept;
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
        // The caller first reserves the reusable physical replica. This only
        // prepares that reusable slice; the professional Handler performs its data conversion/upload.
        [[nodiscard]] RenderResult<DrawListResources*>
        prepareDrawList(
            const CompiledPass&, std::uint32_t slot, std::size_t vertex_bytes, std::size_t index_bytes
        ) noexcept;
        // Copies borrowed frame input into finite staging windows; accepted copies, not GPU completion,
        // release that input. A terminal frame failure must settle the remaining borrow before delivery.
        [[nodiscard]] RenderResult<bool> prepareDrawListUploads(std::uint32_t slot, ResourceUploader&) noexcept;
        void finishDrawListUploads(std::uint32_t slot, RenderResult<void>) noexcept;
        // Caller owns the binding recipe and supplies one stable key per distinct private input set.
        // Only the writable FIF is modified; the returned set remains owned by this instance.
        [[nodiscard]] RenderResult<nri::DescriptorSet*> prepareRasterBindings(
            std::uint32_t slot,
            const PassBindings&,
            std::uint64_t binding_key,
            std::span<const ResourceInput>
        ) noexcept;
        // Remove recipes before their professional Owner is destroyed; GPU views retire by accepted frontiers.
        void releaseRasterBindings(std::span<const PassBindings* const>) noexcept;
        [[nodiscard]] const CompiledPass* compiledPass(GraphPassId) const noexcept;
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

        [[nodiscard]] RenderResult<void>
        bindParameterSource(const GraphAssemblyPlan&, EParameterSource, const rdesc::ShaderBlock&) noexcept;
        [[nodiscard]] RenderResult<void> setParameterSource(EParameterSource, std::span<const std::byte>) noexcept;
        [[nodiscard]] bool usesParameterSource(EParameterSource) const noexcept;
        [[nodiscard]] RenderResult<void> setInputs(
            std::span<const std::byte>,
            std::span<const std::uint8_t>
        ) noexcept;

        [[nodiscard]] RenderResult<void> setConditions(std::span<const std::uint8_t>) noexcept;

        [[nodiscard]] std::span<const std::uint8_t> conditions() const noexcept
        {
            return condition_values_;
        }

        // An empty selection invalidates every declared history at the next preparation boundary.
        [[nodiscard]] RenderResult<void> invalidateHistory(std::span<const GraphResourceId> previous = {}) noexcept;

        [[nodiscard]] std::span<const std::byte> instanceParameters() const noexcept
        {
            return instance_values_;
        }


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
        [[nodiscard]] RenderResult<void> prepareDescriptors(const GraphAssemblyPlan&) noexcept;
        [[nodiscard]] RenderResult<void> rebind(std::uint32_t slot) noexcept;
        void markParameters(std::uint32_t pass, std::uint32_t slot) noexcept;
        void markInstanceParameters(std::uint32_t offset, std::uint32_t bytes) noexcept;
        [[nodiscard]] RenderResult<void> prepareParameters(std::uint32_t slot) noexcept;

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
            std::span<const std::byte> parameters;
            const rdesc::ShaderBlock* parameter_layout = nullptr;
            std::uint64_t parameter_revision = 0;
            std::span<nri::Buffer* const> addressed_buffers;
            std::uint64_t addressed_revision = 0;
            bool dirty = false;
        };

        struct HistoryState;
        struct FrameResources;
        [[nodiscard]] RenderResult<nri::BufferDesc> bufferDescription(const CompiledResource&) const noexcept;
        [[nodiscard]] nri::Result createBufferView(nri::BufferViewDesc, nri::Descriptor*&) const noexcept;
        [[nodiscard]] RenderResult<nri::TextureDesc> textureDescription(const CompiledResource&) const noexcept;
        [[nodiscard]] RenderResult<nri::TextureDesc>
        textureDescription(const CompiledResource&, const TargetDesc&) const noexcept;
        [[nodiscard]] RenderResult<void> createDrawListResources(std::uint32_t slot) noexcept;
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
            RasterState raster_state;
        };

        struct ExternalParameterCopy final
        {
            std::uint32_t resource;
            ParameterByteCopy copy;
        };

        DeviceSession* session_;
        SubmissionFrontiers uses_;
        ResourceRegistry* resources_ = nullptr;

        struct Data final
        {
            std::vector<GraphResourceId> input_slots;
            std::unique_ptr<InstanceData> value;
            std::uint32_t parameter_offset, parameter_bytes;
            std::vector<ResourceInput> inputs;
        };
        std::vector<Data> data_;
        std::vector<FrameResources> frames_;
        std::vector<HistoryState> histories_;
        const RenderGraphExecutable* plan_;
        const NativeGraphPrograms* programs_;
        std::span<const std::uint32_t> source_pass_slots_;
        RasterViewport viewport_;
        TargetOutput output_;

        struct SamplerInput final
        {
            nri::Descriptor* descriptor = nullptr;
            bool comparison = false;
            bool used = false;
        };

        std::vector<SamplerInput> samplers_;
        std::vector<ExternalParameterCopy> external_parameter_copies_;
        std::vector<std::size_t> external_parameter_offsets_;
        struct SourceCopy final
        {
            EParameterSource source;
            std::uint32_t source_offset, destination_offset, bytes;
        };
        std::vector<SourceCopy> source_copies_;
        std::array<std::uint32_t, 3> source_sizes_{};
        std::vector<std::byte> instance_values_;
        std::vector<std::uint8_t> condition_values_;
        ExecutionProgress progress_;
        RenderError submission_error_;
    };
} // namespace lux::render::detail

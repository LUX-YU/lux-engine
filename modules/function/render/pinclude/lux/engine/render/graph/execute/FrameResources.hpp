#pragma once

#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/device/CopyEncoding.hpp>
#include <lux/engine/render/graph/execute/HistoryState.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/graph/execute/FrameResourceBinding.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <algorithm>
#include <atomic>
#include <exception>
#include <utility>

namespace lux::render::detail
{
    struct RasterDescriptorField;
    struct RasterDescriptorLayout;
    struct CompiledRasterStream;

    struct FrozenOperandPatches final
    {
        std::size_t transfer = 0, compute = 0;
    };

    struct FrozenBufferFill final
    {
        nri::Buffer* buffer;
        std::uint64_t offset;
        nri::Descriptor* storage_view;
    };

    struct FrozenImageBuffer final
    {
        nri::Texture* image;
        nri::Buffer* buffer;
        nri::TextureRegionDesc region;
        nri::TextureDataLayoutDesc layout;
        std::uint32_t slice_copies = 1;
    };

    struct TransferOperands final
    {
        std::vector<BufferCopyRegion> buffer_copies;
        std::vector<TextureCopyRegion> image_copies;
        std::vector<FrozenImageBuffer> image_buffers;
        std::vector<FrozenBufferFill> fills;
        std::vector<nri::AttachmentDesc> clears;

        void initialize(const TransferProgram&) noexcept;
    };

    struct FrozenRecordingFrame;

    void recordTransfers(
        const TransferCommands&, const FrozenRecordingFrame&, nri::CommandBuffer&, const nri::CoreInterface&
    ) noexcept;

    struct FrozenBufferAddress final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t offset = 0;
    };

    struct FramePass final
    {
        const CompiledPass* compiled_pass;
        std::optional<nri::GlobalBarrierDesc> alias_barrier;
        nri::DescriptorSet* private_descriptors = nullptr;
        std::span<nri::Descriptor*> private_views;
        std::vector<nri::Descriptor*> history_views;
        std::vector<nri::AttachmentDesc> colors;
        nri::AttachmentDesc depth{};
        std::vector<nri::Descriptor*> clear_views;
        std::span<std::byte> parameters;
        nri::Viewport viewport{};
        nri::Rect scissor{};
        std::uint64_t parameter_source_revision = 0;
    };

    struct DispatchOperands final
    {
        nri::DispatchDesc dispatch{};
        nri::Buffer* indirect_buffer = nullptr;
        std::uint64_t indirect_offset = 0;
    };

    struct RecordHistoryInitialization final
    {
        nri::TextureBarrierDesc image_before{}, image_after{};
        nri::BufferBarrierDesc buffer_before{}, buffer_after{};
        std::span<nri::Descriptor* const> views;
        nri::ClearValue clear_value{};
        std::uint64_t buffer_size = 0;
        bool depth = false;
    };

    // CPU cache state for one native recording batch in a FIF slot. Its allocator and CB are device-owned.
    struct CommandBatch final
    {
        std::uint64_t revision = 1, recorded_revision = 0;
        std::atomic<bool> recording{false};

        void invalidate() noexcept
        {
            if (revision != recorded_revision) return;
            if (revision == UINT64_MAX) std::terminate();
            ++revision;
        }

        CommandBatch() noexcept = default;
        CommandBatch(const CommandBatch&) = delete;
        CommandBatch& operator=(const CommandBatch&) = delete;
        CommandBatch& operator=(CommandBatch&&) = delete;

        CommandBatch(CommandBatch&& other) noexcept
        {
            // Only cold construction may relocate a batch. Published batches keep stable addresses.
            if (other.recording.load(std::memory_order_acquire)) std::terminate();
            revision = other.revision;
            recorded_revision = other.recorded_revision;
        }
    };

    // A synchronous recording borrow of prepared, stable FIF slices. Only the caller owns the
    // mutation window; no owner, scheduler, descriptor directory or History selection is exposed.
    // Parameters and draw bytes already reside in FIF storage, not the client's mutable vectors.
    struct FrozenRecordingFrame final
    {
        const compiled::RenderGraph& plan;
        const TransferOperands& transfers;
        std::span<const FrozenVertexBinding> vertices;
        std::span<const FrozenBufferAddress> raster_addresses;
        std::span<const FramePass> passes;
        std::span<const DispatchOperands> dispatches;
        std::span<const RasterDrawList> draws;
        std::span<const RecordHistoryInitialization> history_initializations;
        std::span<const std::uint8_t> enabled_regions;
        std::span<const SelectedBindingDelta> binding_choices;
        std::span<const nri::TextureBarrierDesc> entry_images;
        std::span<const nri::BufferBarrierDesc> entry_buffers;
        std::span<const nri::TextureBarrierDesc> exit_images;
        std::span<const nri::BufferBarrierDesc> exit_buffers;
        nri::DescriptorPool* descriptor_pool = nullptr;
        std::array<nri::DescriptorSet*, 3> shared_descriptors{};
        std::span<CommandBatch> revisions;
        std::span<const JobFrameResources::NativeBatch> native_batches;
    };

    struct RasterEncoder final
    {
        const nri::CoreInterface& core;
        nri::CommandBuffer& command;
        const FrozenRecordingFrame& frame;
        BindingState& state;

        void beginRendering(const nri::RenderingDesc&, bool initialize_resolve_targets) const noexcept;
        void record(const RasterCommands&, const FramePass&) const noexcept;

    private:
        void bindVertices(std::span<const FrozenVertexBinding>) const noexcept;
        void record(const RasterDrawList&, bool initialize_resolve_targets) const noexcept;
    };

    struct RecordingDependencies;

    // CPU state for one Job and FIF slot. Native ownership stays in the device's stable Job row.
    struct FrameResources final
    {
        explicit FrameResources(JobFrameResources& frame) noexcept : native(frame) {}
        JobFrameResources& native;
        FrameResources(FrameResources&&) noexcept = default;
        FrameResources& operator=(FrameResources&&) = delete;
        FrameResources(const FrameResources&) = delete;
        FrameResources& operator=(const FrameResources&) = delete;

        void initializeRows(const compiled::RenderGraph&) noexcept;
        RenderResult<void> prepareBindings(const compiled::RenderGraph&, DeviceSession&, nri::Descriptor* output_view) noexcept;
        RenderResult<void> createDescriptors(const compiled::RenderGraph&, DeviceSession&, const nri::DescriptorPoolDesc&) noexcept;
        RenderResult<void> rebind(const compiled::RenderGraph&, DeviceSession&) noexcept;
        void prepareAliasBarriers(const PhysicalResourcePlan&) noexcept;
        LUX_RENDER_PUBLIC void invalidatePass(const compiled::RenderGraph&, std::uint32_t pass) noexcept;
        void invalidateResource(const RecordingDependencies&, std::uint32_t resource) noexcept;
        void invalidateRecordings() noexcept;
        void bindHistory(std::span<const HistoryState>, const RecordingDependencies&) noexcept;
        void updateHistoryViews(std::span<const HistoryState>, const nri::CoreInterface&) noexcept;
        void linkRecordInputs(const compiled::RenderGraph&) noexcept;
        [[nodiscard]] bool selectBindingJoin(const compiled::RenderGraph&, std::uint32_t index, bool enabled) noexcept;
        [[nodiscard]] RenderResult<void>
        prepareDescriptorCapacity(JobGpuResources&, std::span<const RasterDrawInput>, bool resize_pool) noexcept;
        [[nodiscard]] RenderResult<void> prepareSections(DeviceSession&, std::span<const RasterDrawInput>) noexcept;
        [[nodiscard]] RenderResult<void> materializeStreams(JobGpuResources&, std::span<const RasterDrawInput>) noexcept;
        [[nodiscard]] RenderResult<void> freeze(std::span<const HistoryState>, const nri::DeviceDesc&) noexcept;
        [[nodiscard]] const FrozenRecordingFrame* recordingFrame() const & noexcept
        {
            return record_ready ? &*linked_recording_ : nullptr;
        }
        const FrozenRecordingFrame* recordingFrame() const && = delete;

    private:
        // Linked after all fixed FIF arrays have stable addresses. Freeze refreshes the varying borrows.
        std::optional<FrozenRecordingFrame> linked_recording_;
        [[nodiscard]] const compiled::RenderGraph& recordingGraph() const noexcept { return linked_recording_->plan; }
        [[nodiscard]] RenderResult<ResourceInput>
        resolveBuffer(const CompiledRasterStream&, const RasterBufferSlice&, EGraphUse) const noexcept;
        [[nodiscard]] RenderResult<RasterDescriptorInput> resolveDescriptor(
            const CompiledRasterStream&, const RasterStreamBinding&, const RasterDescriptorField&, std::uint32_t ordinal
        ) const noexcept;
        [[nodiscard]] RenderResult<void> selectDrawBindings(
            const CompiledRasterStream&, const RasterDrawInput&, const RasterStreamDraw&, const CompiledRasterProgram&
        ) noexcept;
        [[nodiscard]] RenderResult<void> appendDraw(
            const CompiledRasterStream&, const RasterDrawInput&, const RasterStreamDraw&, const CompiledRasterProgram&
        ) noexcept;
        [[nodiscard]] RenderResult<void>
        materializeStream(JobGpuResources&, const CompiledRasterStream&, const RasterDrawInput&) noexcept;
        void publishStream(std::uint32_t pass, std::uint32_t list) noexcept;
        [[nodiscard]] RenderResult<void> resizeLayers(DeviceSession&, JobFrameResources::LayeredImage&) noexcept;
        [[nodiscard]] RenderResult<void> materializeSections(DeviceSession&, std::uint32_t pass, const RasterDrawInput&) noexcept;
        [[nodiscard]] RenderResult<void> growDescriptorPool(JobGpuResources&) noexcept;
        [[nodiscard]] RenderResult<void>
        updateDrawSet(DeviceSession&, std::uint32_t record, std::span<const RasterDescriptorInput>) noexcept;

        void patchDrawSection(std::uint32_t pass, const RasterListCommands&) noexcept;
        void patchRecordPass(const RecordingProgram&, std::uint32_t pass) noexcept;
        void freezeHistoryInitializations(
            std::span<const HistoryState>, std::span<const CompiledResource>, nri::GraphicsAPI
        ) noexcept;
        [[nodiscard]] RenderResult<void>
        patchOperandFields(const RecordingProgram&, const RecordingDependencies&, const nri::DeviceDesc&) noexcept;

        struct PendingBinding final
        {
            std::uint32_t resource;
            ResourceInput value;
        };

        std::vector<PendingBinding> pending_bindings_;

    public:
        [[nodiscard]] bool passEnabled(const CompiledPass& pass) const noexcept
        {
            return pass.condition_region == kGraphInvalidIndex || enabled_regions[pass.condition_region] != 0;
        }

        struct TargetRead final
        {
            std::uint32_t batch, import_index;
            RasterTargetImage requested;
            nri::StageBits wait_stages;
            DescriptorTable::Slot view;
            ResourceQueueUses uses;
        };

        struct TargetBinding final
        {
            std::uint32_t list, binding, argument, read;
            auto key() const noexcept { return std::pair{list, binding}; }
        };
        // Each batch shares a single publication and queue frontier across its descriptor operands for that product.
        std::vector<TargetRead> target_reads;
        std::vector<TargetBinding> target_bindings;
        [[nodiscard]] std::span<const TargetRead> targetReads(std::uint32_t batch) const & noexcept
        {
            return std::ranges::equal_range(target_reads, batch, {}, &TargetRead::batch);
        }

        std::span<const TargetRead> targetReads(std::uint32_t) const && = delete;
        void finishSubmission(const compiled::RenderGraph&, DeviceSession&) noexcept;

        struct RasterProgramDescriptors final
        {
            std::vector<std::uint32_t> records;
            std::uint32_t used = 0;
            std::uint32_t last_binding = 0;

            // The program fixes the field count. Within this traversal, the same first binding
            // selects the same immutable slice, including its frozen Target operands.
            [[nodiscard]] bool select(std::uint32_t first_binding) noexcept
            {
                if (used && first_binding == last_binding) return false;
                last_binding = first_binding;
                ++used;
                return true;
            }
        };
        std::vector<std::vector<RasterProgramDescriptors>> stream_descriptor_rows;
        void releaseRasterBindings(std::uint32_t list, std::vector<nri::Descriptor*>& views) noexcept;
        nri::DescriptorPoolDesc descriptor_capacity{}, descriptor_reserved{};

        std::vector<FrozenVertexBinding> raster_vertices;
        std::vector<FrozenBufferAddress> raster_addresses;
        std::vector<DispatchOperands> dispatches;
        cxx::BasicSparseSet<std::size_t, RecordHistoryInitialization> history_initializations;
        // Deduplicate pending slots without scanning the frame. Retain the complete set on freeze failure.
        class PendingSlots final
        {
        public:
            void initialize(std::size_t count) noexcept
            {
                marked_.resize(count);
                slots_.reserve(count);
                markAll();
            }

            void mark(std::uint32_t slot) noexcept
            {
                if (marked_[slot]) return;
                marked_[slot] = 1;
                slots_.push_back(slot);
            }

            void markAll() noexcept
            {
                for (std::uint32_t i = 0; i < marked_.size(); ++i) mark(i);
            }

            [[nodiscard]] std::span<const std::uint32_t> slots() const noexcept { return slots_; }
            [[nodiscard]] bool contains(std::uint32_t slot) const noexcept { return marked_[slot] != 0; }

            [[nodiscard]] std::span<const std::uint32_t> orderedSlots() noexcept
            {
                if (!std::ranges::is_sorted(slots_)) std::ranges::sort(slots_);
                return slots_;
            }

            void clear() noexcept
            {
                for (const auto slot : slots_) marked_[slot] = 0;
                slots_.clear();
            }

        private:
            std::vector<std::uint8_t> marked_;
            std::vector<std::uint32_t> slots_;
        };

        struct HistoryClear final
        {
            std::uint32_t slot = kGraphInvalidIndex;
            bool initialized = false;
            bool operator==(const HistoryClear&) const noexcept = default;
        };

        PendingSlots pending_passes, pending_operand_fields;
        TransferOperands transfers;
        FrozenOperandPatches last_operands;
        std::size_t last_patched_passes = 0;
        std::vector<std::array<HistoryClear, 2>> history_clears;
        PendingSlots pending_history_clears, pending_history_views;
        bool frozen_for_submission = false;
        void collectRasterViews(nri::Buffer*, nri::Texture*, std::vector<nri::Descriptor*>&) noexcept;
        void retireRasterViews(DeviceSession&, nri::Buffer*, nri::Texture*) noexcept;
        [[nodiscard]] RenderResult<void>
        bindDynamicBuffer(const compiled::RenderGraph&, DeviceSession&, std::size_t buffer_slot, std::uint64_t bytes) noexcept;
        void bindResource(const compiled::RenderGraph&, DeviceSession&, std::uint32_t resource, const ResourceInput&) noexcept;
        // The compiler has qualified the destination and byte extent for this frame layout.
        LUX_RENDER_PUBLIC void applyInlineConstant(
            const compiled::RenderGraph&, CompiledPassSlot, std::span<const std::byte> bytes, std::uint64_t revision
        ) noexcept;
        bool record_ready = false;

        std::vector<FrameResourceBinding> bindings;
        std::vector<FramePass> passes;
        RasterDrawList stream_scratch;
        std::vector<std::uint8_t> stream_layers_seen;
        std::vector<RasterDescriptorInput> stream_binding_inputs;
        std::vector<CommandBatch> command_batches;
        DescriptorTable::Snapshot shared_descriptors;
        std::array<std::uint64_t, 3> shared_set_generations{};

        std::vector<RasterDrawList> raster_draws;
        std::vector<std::byte> parameters;
        std::vector<std::uint8_t> enabled_regions;
        std::vector<SelectedBindingDelta> binding_choices;
        std::vector<std::uint8_t> history_valid;
        std::vector<std::uint8_t> conditions;
        PendingSlots pending_condition_inputs, pending_regions;
        std::uint32_t enabled_pass_count = 0;
        RasterViewport viewport;
        std::vector<std::uint32_t> changed_resources;
        std::size_t unavailable_buffers = 0;

        // Stable addresses of output attachment fields, built once after all Pass arrays are fixed.
        std::vector<nri::Descriptor**> output_views;

        std::vector<BoundMemoryPlacement> memory_bindings;
        // Frame epilogue first, then the compiled per-pass texture exit ranges.
        std::vector<nri::TextureBarrierDesc> entry_images;
        std::vector<nri::BufferBarrierDesc> entry_buffers;
        std::vector<nri::TextureBarrierDesc> exit_images;
        std::vector<nri::BufferBarrierDesc> exit_buffers;
    };

    // One address-stable binding destination per installation. Resize updates the target contract in place.
    struct InstalledGraphBindings final
    {
        const compiled::RenderGraph& graph;
        DeviceSession& device;
        const TargetDesc& target;
        std::vector<FrameResources> frames;

        LUX_RENDER_PUBLIC void bind(GraphResourceId, std::uint32_t slot, const ResourceInput&) noexcept;
        [[nodiscard]] RenderResult<void> qualifyImage(GraphResourceId, const nri::Texture&) const noexcept;
    };

    inline void FrameResources::releaseRasterBindings(std::uint32_t list, std::vector<nri::Descriptor*>& views) noexcept
    {
        if (list >= stream_descriptor_rows.size()) return;
        raster_draws[list].command_revision = 0;
        auto& rows = stream_descriptor_rows[list];
        auto& bindings = native.raster_bindings;
        for (auto& program : rows)
        {
            while (!program.records.empty())
            {
                const auto index = program.records.back();
                auto& removed = bindings[index];
                for (auto* view : removed.views)
                    if (view) views.push_back(view);
                if (index != bindings.size() - 1)
                {
                    removed = std::move(bindings.back());
                    auto& moved = stream_descriptor_rows[removed.list][removed.program];
                    moved.records[removed.ordinal] = index;
                }
                bindings.pop_back();
                program.records.pop_back();
            }
        }
        rows.clear();
    }

} // namespace lux::render::detail

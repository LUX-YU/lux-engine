#pragma once

#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/CopyEncoding.hpp>
#include <lux/engine/render/graph/execute/HistoryState.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/graph/execute/OwnedRasterStream.hpp>
#include <lux/engine/render/graph/execute/FrameResourceBinding.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <atomic>
#include <exception>
#include <utility>

namespace lux::render::detail
{
    struct FrozenOperandPatches final
    {
        std::size_t transfer = 0, compute = 0;
    };

    struct FrozenPatchCounts final
    {
        std::size_t resources = 0, passes = 0;
        bool operator==(const FrozenPatchCounts&) const noexcept = default;
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

    struct BindingState;

    struct PassResourceInput final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t buffer_offset = 0;
        nri::Texture* image = nullptr;
        std::uint32_t width = 0, height = 0;
        std::uint64_t buffer_size = 0;
        bool history_valid = true;
    };

    struct FramePass final
    {
        const CompiledPass* compiled_pass;
        std::vector<nri::TextureBarrierDesc> images;
        std::vector<nri::BufferBarrierDesc> buffers;
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
        std::span<const PassResourceInput> resources;
        std::span<const FramePass> passes;
        std::span<const DispatchOperands> dispatches;
        std::span<const RasterDrawList> draws;
        std::span<const RecordHistoryInitialization> history_initializations;
        std::span<const std::uint8_t> enabled_regions;
        std::span<const SelectedBindingDelta> binding_choices;
        std::span<const nri::TextureBarrierDesc> final_images;
        std::span<const nri::BufferBarrierDesc> final_buffers;
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

        void record(const RasterCommands&, const FramePass&) const noexcept;

    private:
        void record(const RasterDrawList&) const noexcept;
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
        [[nodiscard]] RenderResult<void> freeze(std::span<const HistoryState>, const nri::DeviceDesc&) noexcept;
        [[nodiscard]] const FrozenRecordingFrame* recordingFrame() const & noexcept
        {
            return record_ready ? &*linked_recording_ : nullptr;
        }
        const FrozenRecordingFrame* recordingFrame() const && = delete;

    private:
        // Linked after all fixed FIF arrays have stable addresses. Freeze refreshes the varying borrows.
        std::optional<FrozenRecordingFrame> linked_recording_;
        void patchRecordResource(std::uint32_t index, std::uint32_t history) noexcept;
        void patchDrawSection(std::uint32_t pass, const RasterListCommands&) noexcept;
        void patchRecordPass(const RecordingProgram&, std::uint32_t pass) noexcept;
        void freezeHistoryInitializations(
            std::span<const HistoryState>, std::span<const CompiledResource>, nri::GraphicsAPI
        ) noexcept;
        [[nodiscard]] RenderResult<void>
        patchOperandFields(const RecordingProgram&, const RecordingDependencies&, const nri::DeviceDesc&) noexcept;

    public:
        struct FrozenProjection final
        {
            std::uint32_t resource;
            ResourceQueueUses uses;
        };

        // Usually empty. Keep the original receipt destinations when an owner republishes during submission.
        std::vector<FrozenProjection> replaced_projections;
        [[nodiscard]] ResourceQueueUses selectedUses(std::uint32_t resource) const noexcept;
        void preserveUses(std::uint32_t resource) noexcept;

        void initializeRasterStreams(const RecordingDependencies&) noexcept;

        struct StreamDescriptorRows final
        {
            struct Program final
            {
                std::vector<std::uint32_t> records;
                std::uint32_t used = 0;
            };

            struct Write final
            {
                std::size_t first_input;
                std::uint32_t draw, program, record;
            };

            std::vector<Program> programs;
            std::vector<Write> writes;
            std::size_t input_count = 0;
        };

        std::vector<StreamDescriptorRows> stream_descriptor_rows;
        void releaseRasterBindings(std::uint32_t list, std::vector<nri::Descriptor*>& views) noexcept;
        nri::DescriptorPoolDesc descriptor_capacity{}, descriptor_reserved{};

        std::vector<PassResourceInput> record_resources;
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

        PendingSlots pending_resources, pending_passes, pending_operand_fields;
        TransferOperands transfers;
        FrozenOperandPatches last_operands;
        FrozenPatchCounts last_patches;
        std::vector<std::array<HistoryClear, 2>> history_clears;
        PendingSlots pending_history_clears, pending_history_views;
        bool frozen_for_submission = false;
        void retireRasterViews(DeviceSession&, nri::Buffer*, nri::Texture*) noexcept;
        void bindResource(const compiled::RenderGraph&, DeviceSession&, std::uint32_t resource, const ResourceInput&) noexcept;
        // The compiler has qualified the destination and byte extent for this frame layout.
        LUX_RENDER_PUBLIC void applyInlineConstant(
            const compiled::RenderGraph&, CompiledPassSlot, std::span<const std::byte> bytes, std::uint64_t revision
        ) noexcept;
        bool record_ready = false;

        std::vector<FrameResourceBinding> bindings;
        std::vector<FramePass> passes;
        std::vector<OwnedRasterStream> stream_inputs;
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
        std::vector<std::uint8_t> publish_history;
        std::uint32_t enabled_pass_count = 0;
        RasterViewport viewport;
        std::vector<std::uint32_t> changed_resources;
        std::size_t unavailable_buffers = 0;

        struct LayeredImage final
        {
            GraphResourceId resource;
            std::vector<nri::Descriptor*> views; // Non-owning index into this frame's owned view collection.
            std::uint32_t requested_layers = 0;
        };

        std::vector<LayeredImage> layered_images;
        // Stable addresses of output attachment fields, built once after all Pass arrays are fixed.
        std::vector<nri::Descriptor**> output_views;

        std::vector<BoundMemoryPlacement> memory_bindings;
        std::vector<nri::TextureBarrierDesc> final_images;
        std::vector<nri::BufferBarrierDesc> final_buffers;
    };

    inline void FrameResources::releaseRasterBindings(std::uint32_t list, std::vector<nri::Descriptor*>& views) noexcept
    {
        if (list >= stream_descriptor_rows.size()) return;
        auto& rows = stream_descriptor_rows[list];
        auto& bindings = native.raster_bindings;
        for (auto& program : rows.programs)
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
                    auto& moved = stream_descriptor_rows[removed.list].programs[removed.program.program];
                    moved.records[removed.ordinal] = index;
                }
                bindings.pop_back();
                program.records.pop_back();
            }
        }
        rows.programs.clear();
        rows.writes.clear();
        rows.input_count = 0;
    }

} // namespace lux::render::detail

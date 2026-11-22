#pragma once

#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/graph/execute/FrameRecordInput.hpp>
#include <lux/engine/render/graph/execute/HistoryState.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/graph/execute/OwnedRasterStream.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <atomic>
#include <exception>
#include <utility>

namespace lux::render::detail
{
    struct FrameResourceBinding final
    {
        // NRI returns descriptions owned by the selected native objects.
        void select(nri::Buffer* next_buffer, nri::Texture* next_image, const nri::CoreInterface& core) noexcept
        {
            if (!next_buffer) buffer_description = nullptr;
            else if (buffer != next_buffer || !buffer_description)
                buffer_description = &core.GetBufferDesc(*next_buffer);
            if (!next_image) image_description = nullptr;
            else if (image != next_image || !image_description)
                image_description = &core.GetTextureDesc(*next_image);
            buffer = next_buffer;
            image = next_image;
        }

        const nri::BufferDesc* buffer_description = nullptr;
        const nri::TextureDesc* image_description = nullptr;
        nri::Texture* image = nullptr;
        nri::Descriptor* view = nullptr;
        nri::Buffer* buffer = nullptr;
        std::uint64_t buffer_offset = 0, buffer_size = 0;
        std::span<nri::Buffer* const> addressed_buffers;
        std::uint64_t addressed_revision = 0;
        ResourceQueueUses queue_uses{};
        bool dirty = false;
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
        std::uint64_t parameter_source_revision = 0;
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

        void invalidateImportedBindings() noexcept;
        void invalidatePass(const compiled::RenderGraph&, std::uint32_t pass) noexcept;
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
        void prepareDrawReads() noexcept;
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
        nri::DescriptorPoolDesc descriptor_capacity{}, descriptor_reserved{};

        std::vector<PassResourceInput> record_resources;
        std::vector<RecordPass> record_passes;
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
        bool record_ready = false;

        std::vector<FrameResourceBinding> bindings;
        // Changes when a potentially shared imported identity/range changes. Private presentation rotation
        // updates bindings and barriers separately; resource contents do not invalidate dependencies.
        std::uint64_t imported_revision = 0;
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

} // namespace lux::render::detail

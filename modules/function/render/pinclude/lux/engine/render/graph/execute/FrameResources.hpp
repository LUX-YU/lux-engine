#pragma once

#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/graph/execute/FrameRecordInput.hpp>
#include <lux/engine/render/graph/execute/HistoryState.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <atomic>
#include <exception>
#include <utility>

namespace lux::render::detail
{
    struct RecordingDependencies;
    struct CopyAlignment;

    // CPU state for one Job and FIF slot. Native ownership stays in the device's stable Job row.
    struct RenderGraphInstance::FrameResources final
    {
        explicit FrameResources(JobFrameResources& frame) noexcept : native(frame) {}
        JobFrameResources& native;
        FrameResources(FrameResources&&) noexcept = default;
        FrameResources& operator=(FrameResources&&) = delete;
        FrameResources(const FrameResources&) = delete;
        FrameResources& operator=(const FrameResources&) = delete;

        void invalidateImportedBindings() noexcept;
        [[nodiscard]] RenderResult<void>
        patchTransferFields(const TransferProgram&, const RecordingDependencies&, CopyAlignment) noexcept;

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

            std::vector<Program> programs;
            std::vector<std::uint32_t> draws;
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

        PendingSlots pending_resources, pending_passes, pending_transfer_fields;
        TransferOperands transfers;
        std::size_t patched_transfer_fields = 0;
        FrozenPatchCounts last_patches;
        std::vector<std::array<HistoryClear, 2>> history_clears;
        PendingSlots pending_history_clears, pending_history_views;
        bool record_ready = false;

        std::vector<Binding> bindings;
        // Changes when a potentially shared imported identity/range changes. Private presentation rotation
        // updates bindings and barriers separately; resource contents do not invalidate dependencies.
        std::uint64_t imported_revision = 0;
        std::vector<Pass> passes;
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

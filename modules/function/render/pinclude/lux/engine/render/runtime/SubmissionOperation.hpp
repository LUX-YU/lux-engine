#pragma once

#include <lux/cxx/core/function_ref.hpp>

#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/extensions/graph/Diagnostics.hpp>
#include <lux/engine/render/extensions/graph/RenderGraph.hpp>
#include <optional>
#include <lux/engine/render/runtime/FrameProgress.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render::detail
{
    class RenderGraphInstance;
    class SubmissionQueue;

    struct SubmissionOrder final
    {
        std::uint32_t producer;
        std::uint32_t consumer;
        bool operator==(const SubmissionOrder&) const noexcept = default;
    };

    // One round's frozen graph execution. Workspace contains recyclable FIF scratch and receipts only.
    class LUX_RENDER_PUBLIC SubmissionOperation final
    {
        struct InstanceExecution final
        {
            RenderGraphInstance* instance = nullptr;
            std::uint64_t binding_revision = 0;
            std::uint32_t first_batch = 0;
            std::uint32_t remaining_batches = 0;
        };

        struct BatchExecution final
        {
            std::uint32_t instance = 0, local_batch = 0, queue = 0;
            std::uint32_t predecessor_count = 0, pending_predecessors = 0;
            std::uint32_t first_successor = 0, successor_count = 0;
            SubmissionFrontiers waits;
            std::vector<nri::StageBits> stages;
            std::uint64_t serial = 0;
        };

        struct BatchDependency final
        {
            std::uint32_t producer = 0, consumer = 0;
            nri::StageBits stages = nri::StageBits::ALL;
        };

        struct ImportedResourceAccess final
        {
            const void* physical = nullptr;
            VGraphRange range;
            std::uint32_t instance = 0;
            std::uint32_t batch = 0, release_batch = 0;
            bool write = false;
            bool target_output = false;
            GraphResourceId logical{kGraphInvalidIndex};
        };

        struct Slot final
        {
            std::uint32_t submitted_batches = 0;
            std::uint32_t recorded_instances = 0, recorded_passes = 0;
            std::uint64_t graphics_serial = 0;
            std::uint64_t preparation_serial = 0;
            std::uint64_t uniform_upload_serial = 0;
            std::vector<InstanceExecution> instances;
            std::vector<SubmissionOrder> order;
            std::vector<BatchExecution> batches;
            std::vector<BatchDependency> dependencies;
            std::vector<std::vector<std::uint32_t>> ready_batches;
            std::vector<ImportedResourceAccess> imported_uses;
            std::vector<std::uint32_t> recording_batches;
            struct Recording final
            {
                std::span<nri::CommandBuffer* const> commands;
                RenderError error;
            };
            std::vector<Recording> recordings;
            std::vector<nri::CommandBuffer*> recording_commands;
            std::vector<nri::FenceSubmitDesc> wait_fences, signal_fences;
            std::uint32_t remaining_batches = 0;
            bool graph_started = false;
            std::uint64_t dependency_rebuilds = 0, imported_comparisons = 0;
            std::vector<std::uint64_t> imported_group_counts;
            std::uint64_t imported_overlap_ns = 0;
        };

    public:
        struct InstanceSubmissionResult final
        {
            bool wrote_output = false;
            bool completed = false;
            std::uint32_t first_batch = kGraphInvalidIndex;
        };

        struct RasterReads final
        {
            std::uint64_t buffer_ranges = 0, buffer_bytes = 0, image_ranges = 0, image_subresources = 0;
            std::uint64_t geometry_ranges = 0, geometry_bytes = 0;
        };

        class LUX_RENDER_PUBLIC Workspace final
        {
        public:
            Workspace(std::uint32_t slots, std::size_t queues) noexcept;
            [[nodiscard]] std::uint32_t submittedBatches(std::uint32_t slot) const noexcept;
            [[nodiscard]] InstanceSubmissionResult acceptedFrameUse(
                std::uint32_t, const RenderGraphInstance&, SubmissionFrontiers&
            ) const noexcept;
            [[nodiscard]] bool acceptedPass(
                std::uint32_t slot, const RenderGraphInstance&, InstanceSubmissionResult, std::uint32_t pass
            ) const noexcept;
            [[nodiscard]] SubmissionFrontiers acceptedUses(
                const DeviceSession&, const FrameProgress&, std::span<RenderGraphInstance* const>
            ) const noexcept;
            [[nodiscard]] bool dumpSubmissions(const FrameProgress&, DumpSink) const noexcept;
            [[nodiscard]] RasterReads takeRasterReads() noexcept;

        private:
            friend class SubmissionOperation;
            std::vector<Slot> slots_;
            RasterReads raster_reads_;
            std::uint32_t queue_cursor_ = 0;
        };

        SubmissionOperation(DeviceSession&, FrameProgress&, FrameProgress::SlotSelection, Workspace&) noexcept;
        [[nodiscard]] RenderResult<void>
        prepare(std::span<RenderGraphInstance* const>, std::span<const SubmissionOrder>) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> record(execution::CpuScheduler, bool parallel) noexcept;
        using BatchAccepted = cxx::function_ref<void(const RenderGraphInstance&, std::uint32_t, SubmissionPoint) noexcept>;

        [[nodiscard]] exec::task<RenderResult<std::uint32_t>>
        submit(const std::optional<RenderError>& cancel, std::optional<BatchAccepted> accepted = {}) noexcept;
        void acceptPreparation(std::uint64_t serial) noexcept;
        void waitForUniformUpload(std::uint64_t serial) noexcept
        {
            slot_.uniform_upload_serial = std::max(slot_.uniform_upload_serial, serial);
        }
        void rejectSuffix(RenderError) noexcept;
        [[nodiscard]] bool complete() const noexcept { return slot_.remaining_batches == 0; }
        [[nodiscard]] bool prepared() const noexcept { return slot_.graph_started; }
        [[nodiscard]] bool canSubmit() const noexcept;
        [[nodiscard]] bool refersTo(const RenderGraphInstance&) const noexcept;

    private:
        [[nodiscard]] static bool laterBatch(const Slot&, std::uint32_t, std::uint32_t) noexcept;
        [[nodiscard]] std::uint32_t readyQueue() const noexcept;
        void accept(const SubmissionQueue&, std::uint64_t serial) noexcept;
        void enqueueReadyBatch(Slot&, std::uint32_t) noexcept;
        void selectReadyBatches(Slot&, std::uint32_t) noexcept;
        void acceptBatch(Slot&, std::uint32_t, std::uint64_t, std::optional<BatchAccepted>) noexcept;
        [[nodiscard]] bool hasPresentationBoundary(const BatchExecution&) const noexcept;
        [[nodiscard]] RenderResult<void>
        rebuildDependencies(std::span<RenderGraphInstance* const>, std::span<const SubmissionOrder>) noexcept;
        [[nodiscard]] RenderResult<void> appendImportedDependencies(std::span<RenderGraphInstance* const>) noexcept;
        [[nodiscard]] std::uint32_t slotIndex() const noexcept { return selection_.frame.slot_index; }

        DeviceSession& device_;
        FrameProgress& progress_;
        FrameProgress::SlotSelection selection_;
        Workspace& workspace_;
        Slot& slot_;
    };
} // namespace lux::render::detail

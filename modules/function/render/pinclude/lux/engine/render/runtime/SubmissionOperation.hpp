#pragma once

#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/cxx/core/function_ref.hpp>

#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/graph/Diagnostics.hpp>
#include <lux/engine/render/graph/compile/GraphDefinition.hpp>
#include <optional>
#include <lux/engine/render/runtime/FrameProgress.hpp>
#include <lux/engine/render/graph/Ids.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render::runtime_detail { struct JobExecution; }

namespace lux::render::detail
{
    class SubmissionQueue;
    struct FrozenRecordingFrame;

    struct SubmissionOrder final
    {
        std::uint32_t producer;
        std::uint32_t consumer;
        std::uint32_t target_input = kGraphInvalidIndex; // Whole-Job order when no sampled Target operand is selected.
        bool operator==(const SubmissionOrder&) const noexcept = default;
    };

    // One round's frozen graph execution. Workspace contains recyclable FIF scratch and receipts only.
    class LUX_RENDER_PUBLIC SubmissionOperation final
    {
    public:
        struct JobSubmissionResult final
        {
            bool wrote_output = false;
            bool completed = false;
            std::uint32_t first_batch = kGraphInvalidIndex;
            SubmissionFrontiers accepted;
        };

    private:
        struct JobReceipt final
        {
            ExecutionBindingId binding{};
            JobSubmissionResult result;
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

        struct Slot final
        {
            Slot() noexcept = default;
            Slot(const Slot&) = delete;
            Slot& operator=(const Slot&) = delete;

            std::uint32_t submitted_batches = 0;
            std::uint32_t recorded_instances = 0, recorded_passes = 0;
            std::uint64_t graphics_serial = 0;
            std::uint64_t preparation_serial = 0;
            std::vector<JobReceipt> jobs;
            std::vector<SubmissionOrder> order;
            std::vector<BatchExecution> batches;
            std::vector<BatchDependency> dependencies;
            std::vector<std::vector<std::uint32_t>> ready_batches;
            std::vector<std::uint32_t> recording_batches;
            struct Recording final
            {
                std::span<nri::CommandBuffer* const> commands;
                RenderError error;
            };
            std::vector<const FrozenRecordingFrame*> recording_frames;
            std::vector<Recording> recordings;
            std::vector<nri::CommandBuffer*> recording_commands;
            std::vector<nri::FenceSubmitDesc> wait_fences, signal_fences;
            std::uint32_t remaining_batches = 0;
            bool graph_started = false;
            std::uint64_t dependency_rebuilds = 0;
        };

    public:
        struct RasterReads final
        {
            std::uint64_t buffer_ranges = 0, buffer_bytes = 0, image_ranges = 0, image_subresources = 0;
            std::uint64_t geometry_ranges = 0, geometry_bytes = 0;
        };

        class LUX_RENDER_PUBLIC Workspace final
        {
        public:
            Workspace(std::uint32_t slots, std::size_t queues) noexcept;
            Workspace(const Workspace&) = delete;
            Workspace& operator=(const Workspace&) = delete;
            [[nodiscard]] std::uint32_t submittedBatches(std::uint32_t slot) const noexcept;
            [[nodiscard]] const JobSubmissionResult*
            acceptedFrameUse(std::uint32_t slot, std::size_t ordinal, ExecutionBindingId) const & noexcept;
            const JobSubmissionResult* acceptedFrameUse(std::uint32_t, std::size_t, ExecutionBindingId) const && = delete;
            [[nodiscard]] bool acceptedPass(
                std::uint32_t slot, const runtime_detail::JobExecution&, const JobSubmissionResult&, std::uint32_t pass
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
        ~SubmissionOperation() noexcept;
        [[nodiscard]] RenderResult<void>
        prepare(std::span<runtime_detail::JobExecution* const>, std::span<const SubmissionOrder>) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> record(execution::CpuScheduler, bool parallel) noexcept;
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> submit(const std::optional<RenderError>& cancel) noexcept;
        void acceptPreparation(std::uint64_t serial) noexcept;
        void rejectSuffix(RenderError) noexcept;
        [[nodiscard]] bool complete() const noexcept { return slot_.remaining_batches == 0; }
        [[nodiscard]] bool prepared() const noexcept { return slot_.graph_started; }
        [[nodiscard]] bool canSubmit() const noexcept;

    private:
        [[nodiscard]] static bool laterBatch(const Slot&, std::uint32_t, std::uint32_t) noexcept;
        [[nodiscard]] std::uint32_t readyQueue() const noexcept;
        void accept(const SubmissionQueue&, std::uint64_t serial) noexcept;
        void enqueueReadyBatch(Slot&, std::uint32_t) noexcept;
        void selectReadyBatches(Slot&, std::uint32_t) noexcept;
        void acceptBatch(Slot&, std::uint32_t, std::uint64_t) noexcept;
        [[nodiscard]] bool hasPresentationBoundary(const BatchExecution&) const noexcept;
        [[nodiscard]] RenderResult<void>
        rebuildDependencies(std::span<const SubmissionOrder>) noexcept;
        [[nodiscard]] std::uint32_t slotIndex() const noexcept { return selection_.frame.slot_index; }

        DeviceSession& device_;
        FrameProgress& progress_;
        FrameProgress::SlotSelection selection_;
        Workspace& workspace_;
        Slot& slot_;
        std::span<runtime_detail::JobExecution* const> jobs_;
    };
} // namespace lux::render::detail

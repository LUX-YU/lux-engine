#pragma once

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

        struct ImportedUse final
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
            std::vector<InstanceExecution> instances;
            std::vector<SubmissionOrder> order;
            std::vector<BatchExecution> batches;
            std::vector<BatchDependency> dependencies;
            std::vector<std::vector<std::uint32_t>> ready_batches;
            std::vector<ImportedUse> imported_uses;
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
        struct InstanceUse final
        {
            bool wrote_output = false;
            bool completed = false;
        };

        class LUX_RENDER_PUBLIC Workspace final
        {
        public:
            Workspace(std::uint32_t slots, std::size_t queues) noexcept;
            [[nodiscard]] std::uint32_t submittedBatches(std::uint32_t slot) const noexcept;
            [[nodiscard]] InstanceUse acceptedFrameUse(
                std::uint32_t, const RenderGraphInstance&, SubmissionFrontiers&
            ) const noexcept;
            [[nodiscard]] SubmissionFrontiers acceptedUses(
                const DeviceSession&, const FrameProgress&, std::span<RenderGraphInstance* const>
            ) const noexcept;
            [[nodiscard]] bool dumpSubmissions(const FrameProgress&, DumpSink) const noexcept;

        private:
            friend class SubmissionOperation;
            std::vector<Slot> slots_;
            std::uint32_t queue_cursor_ = 0;
        };

        SubmissionOperation(DeviceSession&, FrameProgress::Round&, Workspace&) noexcept;
        [[nodiscard]] RenderResult<void>
        prepare(std::span<RenderGraphInstance* const>, std::span<const SubmissionOrder>) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> record(execution::CpuScheduler) noexcept;
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> submit(const std::optional<RenderError>& cancel) noexcept;
        void acceptPreparation(std::uint64_t serial) noexcept;
        void rejectSuffix(RenderError) noexcept;
        [[nodiscard]] bool complete() const noexcept { return slot_.remaining_batches == 0; }
        [[nodiscard]] bool prepared() const noexcept { return slot_.graph_started; }
        [[nodiscard]] bool canSubmit() const noexcept;
        [[nodiscard]] bool refersTo(const RenderGraphInstance&) const noexcept;

    private:
        [[nodiscard]] static bool laterBatch(const Slot&, std::uint32_t, std::uint32_t) noexcept;
        [[nodiscard]] std::uint32_t readyQueue() const noexcept;
        void accept(const SubmissionQueue&) noexcept;
        void enqueueReadyBatch(Slot&, std::uint32_t) noexcept;
        void selectReadyBatches(Slot&, std::uint32_t) noexcept;
        void acceptBatch(Slot&, std::uint32_t, std::uint64_t) noexcept;
        [[nodiscard]] bool hasPresentationBoundary(const BatchExecution&) const noexcept;
        [[nodiscard]] RenderResult<void>
        rebuildDependencies(std::span<RenderGraphInstance* const>, std::span<const SubmissionOrder>) noexcept;
        [[nodiscard]] RenderResult<void> appendImportedDependencies(std::span<RenderGraphInstance* const>) noexcept;
        [[nodiscard]] std::uint32_t slotIndex() const noexcept { return round_.context().slot_index; }

        DeviceSession& device_;
        FrameProgress::Round& round_;
        Workspace& workspace_;
        Slot& slot_;
    };
} // namespace lux::render::detail

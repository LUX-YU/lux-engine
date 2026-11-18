#pragma once

#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/upload/Preparation.hpp>
#include <lux/engine/render/runtime/SubmissionOperation.hpp>

namespace lux::render::detail
{
    // One accepted device round. All CPU and native submissions return before the round releases its slot.
    class LUX_RENDER_PUBLIC FrameOperation final
    {
        struct PreparedStorage final
        {
            ResourceInstanceId identity;
            std::uint64_t revision = 0;
        };

    public:
        struct LUX_RENDER_PUBLIC Workspace final
        {
            Workspace(std::uint32_t slots, std::size_t queues, std::size_t preparation_capacity) noexcept;

            SubmissionOperation::Workspace submissions;
            Preparation preparation;
            std::vector<std::vector<PreparedStorage>> replicas;
            SubmissionFrontiers blocked_completion;
        };

        FrameOperation(
            DeviceSession&, ResourceUploader*, FrameProgress&, Workspace&, execution::CpuScheduler,
            bool parallel_recording = true
        ) noexcept;
        ~FrameOperation() noexcept;
        FrameOperation(const FrameOperation&) = delete;
        FrameOperation& operator=(const FrameOperation&) = delete;

        [[nodiscard]] RenderResult<bool> tryBegin() noexcept;
        [[nodiscard]] bool active() const noexcept { return round_.active(); }
        [[nodiscard]] bool canRun() const noexcept;
        [[nodiscard]] FrameContext context() const noexcept { return round_.context(); }
        [[nodiscard]] bool submitting() const noexcept { return active() && submission_->prepared(); }
        [[nodiscard]] bool submitting(const RenderGraphInstance& instance) const noexcept
        {
            return submitting() && submission_->refersTo(instance);
        }
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> run(
            std::span<RenderGraphInstance* const>, std::span<const StorageFrameUse>,
            std::span<const SubmissionOrder> order = {}
        ) noexcept;
        void setRecordingPolicy(execution::CpuScheduler cpu, bool parallel) noexcept
        {
            cpu_ = cpu;
            parallel_recording_ = parallel;
        }

        [[nodiscard]] bool parallelRecordingPending() const noexcept
        {
            return parallel_recording_ && recording_pending_;
        }

        void cancel(RenderError) noexcept;

    private:
        void finish() noexcept;
        void settlePreparation(RenderResult<void>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareStorages(std::span<const StorageFrameUse>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareDrawLists(std::span<RenderGraphInstance* const>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareInstances(std::span<RenderGraphInstance* const>) noexcept;
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> submitPreparation() noexcept;
        [[nodiscard]] std::uint32_t slot() const noexcept { return round_.context().slot_index; }

        DeviceSession& device_;
        ResourceUploader* uploads_;
        FrameProgress& progress_;
        Workspace& workspace_;
        execution::CpuScheduler cpu_;
        bool parallel_recording_;
        FrameProgress::Round round_;
        std::optional<SubmissionOperation> submission_;
        std::optional<RenderError> cancellation_;
        std::size_t prepared_instances_ = 0;
        bool worker_pending_ = false;
        bool recording_pending_ = false;
        bool copies_flushed_ = false;
        bool resources_ready_ = false;
        bool draw_data_ready_ = false;
        bool waiting_for_gpu_ = false;
    };
} // namespace lux::render::detail

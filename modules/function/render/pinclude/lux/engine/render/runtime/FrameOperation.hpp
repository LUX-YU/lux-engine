#pragma once

#include <exec/async_scope.hpp>
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/resource/ConstantBufferStorage.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/graph/PreparationPlan.hpp>
#include <lux/engine/render/runtime/SubmissionOperation.hpp>

namespace lux::render::detail
{
    class DeviceEntry;

    // The owner admits compiler work after recordings already using the entire pool have joined.
    class RecordingWorkers final
    {
    public:
        explicit RecordingWorkers(execution::CpuScheduler independent) noexcept : independent_(independent) {}

        [[nodiscard]] auto beginCompilation() noexcept
        {
            ++compilations_;
            return recordings_.on_empty();
        }

        void endCompilation() noexcept
        {
            if (compilations_ == 0) std::terminate();
            --compilations_;
        }

    private:
        friend class FrameOperation;
        execution::CpuScheduler independent_;
        exec::async_scope recordings_;
        std::size_t compilations_ = 0;
    };

    // One accepted device round. All CPU and native submissions return before the round releases its slot.
    class LUX_RENDER_PUBLIC FrameOperation final
    {
        struct PreparedStorage final
        {
            ResourceInstanceId identity;
            std::uint64_t revision = 0;
        };

        struct ResourceParticipant final
        {
            ResourceInstanceId resource;
            std::size_t replica;
            ReplicaFrame frame;
            ReplicaCopyRange ranges;
            std::uint64_t revision = 0;
        };

        struct ProducerParticipant final
        {
            RenderJobId job;
            std::uint64_t generation;
            ReplicaFrame frame;
            std::size_t first, count;
        };

    public:
        struct LUX_RENDER_PUBLIC Workspace final
        {
            Workspace(std::uint32_t slots, std::size_t queues, std::size_t preparation_capacity) noexcept;

            SubmissionOperation::Workspace submissions;
            std::vector<RenderGraphInstance*> frozen_instances;
            std::vector<std::vector<PreparedStorage>> replicas;
            std::vector<ResourceParticipant> resource_participants;
            std::vector<ProducerParticipant> producer_participants;
            std::vector<RenderGraphInstance*> producer_instances;
            std::vector<PreparedBufferCopy> prepared_buffers;
            SubmissionFrontiers blocked_completion;
            ConstantBufferSelections buffers;
            std::vector<DescriptorTable::NativeBindingSelection> native_bindings;
        };

        FrameOperation(DeviceEntry&, execution::CpuScheduler) noexcept;
        FrameOperation(DeviceSession&, FrameProgress&, Workspace&, execution::CpuScheduler) noexcept;
        ~FrameOperation() noexcept;
        FrameOperation(const FrameOperation&) = delete;
        FrameOperation& operator=(const FrameOperation&) = delete;

        [[nodiscard]] RenderResult<bool> tryBegin() noexcept;
        [[nodiscard]] bool active() const noexcept { return selection_.has_value(); }
        [[nodiscard]] bool canRun() const noexcept;
        [[nodiscard]] FrameContext context() const noexcept
        {
            if (!selection_) std::terminate();
            return selection_->frame;
        }
        [[nodiscard]] bool submitting() const noexcept { return active() && submission_->prepared(); }
        [[nodiscard]] bool submitting(const RenderGraphInstance& instance) const noexcept
        {
            return submitting() && submission_->refersTo(instance);
        }
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> run(
            std::span<RenderGraphInstance* const>, std::span<const ResourceInstanceId>,
            std::span<const SubmissionOrder> order = {}
        ) noexcept;
        void useRecordingWorkers(RecordingWorkers& workers) noexcept { workers_ = &workers; }

        void cancel(RenderError) noexcept;

    private:
        enum class EPhase : std::uint8_t
        {
            RESOURCES,
            PRODUCER_SELECTIONS,
            PRODUCER_UPLOADS,
            FREEZE,
            RECORDING,
            SUBMITTING
        };

        enum class EPreparationStatus : std::uint8_t { READY, AWAITING_PROGRESS };

        void finish() noexcept;
        void settlePreparation(SubmissionPoint accepted = {}, std::size_t ranges = 0, RenderError error = {}) noexcept;
        [[nodiscard]] std::pair<void*, const StorageBindingOps*> frameStorage(ResourceInstanceId) const noexcept;

        [[nodiscard]] ReplicaFrame replicaFrame() const noexcept { return {slot(), selection_->epoch}; }
        [[nodiscard]] RenderResult<EPreparationStatus>
        prepareStorages(std::span<RenderGraphInstance* const>, std::span<const ResourceInstanceId>) noexcept;
        [[nodiscard]] RenderResult<EPreparationStatus> prepareInstances(std::span<RenderGraphInstance* const>) noexcept;
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> submitPreparation() noexcept;
        [[nodiscard]] std::uint32_t slot() const noexcept { return context().slot_index; }

        DeviceSession& device_;
        ResourceUploader* uploads_;
        FrameProgress& progress_;
        Workspace& workspace_;
        execution::CpuScheduler cpu_;
        RecordingWorkers* workers_ = nullptr;
        DeviceEntry* owner_;
        std::optional<FrameProgress::SlotSelection> selection_;
        EPhase phase_ = EPhase::RESOURCES;
        std::optional<SubmissionOperation> submission_;
        std::optional<RenderError> cancellation_;
        bool worker_pending_ = false;
        bool settling_ = false;
        bool copies_flushed_ = false;
        bool waiting_for_gpu_ = false;
    };
} // namespace lux::render::detail

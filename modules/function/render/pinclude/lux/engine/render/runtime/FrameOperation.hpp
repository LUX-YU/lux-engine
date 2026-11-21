#pragma once

#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/effect/UniformBlockVersions.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/graph/PreparationPlan.hpp>
#include <lux/engine/render/runtime/SubmissionOperation.hpp>

namespace lux::render::detail
{
    class DeviceEntry;

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
            std::vector<std::vector<PreparedStorage>> replicas;
            std::vector<ResourceParticipant> resource_participants;
            std::vector<ProducerParticipant> producer_participants;
            std::vector<RenderGraphInstance*> producer_instances;
            std::vector<PreparedBufferCopy> prepared_buffers;
            SubmissionFrontiers blocked_completion;
            SubmissionFrontiers uniform_completed;
            std::vector<SelectedUniformVersion> uniforms;
            std::vector<DescriptorTable::NativeBindingSelection> native_bindings;
        };

        FrameOperation(
            DeviceSession&, ResourceUploader*, FrameProgress&, Workspace&, execution::CpuScheduler,
            bool parallel_recording = true, DeviceEntry* owner = nullptr
        ) noexcept;
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
        using UniformStateLookup = cxx::function_ref<const void*(EffectId, graph::NodeId, cxx::basic_type_info) noexcept>;

        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> run(
            std::span<RenderGraphInstance* const>, std::span<const ResourceInstanceId>,
            std::span<const SubmissionOrder> order = {}, std::optional<UniformStateLookup> uniform_state = {}
        ) noexcept;
        void setRecordingPolicy(execution::CpuScheduler cpu, bool parallel) noexcept
        {
            cpu_ = cpu;
            parallel_recording_ = parallel;
        }

        [[nodiscard]] bool parallelRecordingPending() const noexcept
        {
            return parallel_recording_ && worker_pending_ && phase_ == EPhase::RECORDING;
        }

        void cancel(RenderError) noexcept;

    private:
        enum class EPhase : std::uint8_t
        {
            UNIFORMS,
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
        prepareUniforms(std::span<RenderGraphInstance* const>, std::optional<UniformStateLookup>) noexcept;
        [[nodiscard]] RenderResult<EPreparationStatus> prepareStorages(std::span<const ResourceInstanceId>) noexcept;
        [[nodiscard]] RenderResult<EPreparationStatus> prepareInstances(std::span<RenderGraphInstance* const>) noexcept;
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> submitPreparation() noexcept;
        [[nodiscard]] std::uint32_t slot() const noexcept { return context().slot_index; }

        DeviceSession& device_;
        ResourceUploader* uploads_;
        FrameProgress& progress_;
        Workspace& workspace_;
        execution::CpuScheduler cpu_;
        bool parallel_recording_;
        DeviceEntry* owner_;
        std::optional<FrameProgress::SlotSelection> selection_;
        EPhase phase_ = EPhase::UNIFORMS;
        std::optional<SubmissionOperation> submission_;
        std::optional<RenderError> cancellation_;
        std::size_t prepared_instances_ = 0;
        bool worker_pending_ = false;
        bool settling_ = false;
        bool copies_flushed_ = false;
        bool waiting_for_gpu_ = false;
    };
} // namespace lux::render::detail

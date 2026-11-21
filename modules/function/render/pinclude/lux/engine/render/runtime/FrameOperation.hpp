#pragma once

#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/effect/UniformBlockVersions.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/upload/Preparation.hpp>
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

    public:
        struct LUX_RENDER_PUBLIC Workspace final
        {
            Workspace(std::uint32_t slots, std::size_t queues, std::size_t preparation_capacity) noexcept;

            SubmissionOperation::Workspace submissions;
            Preparation preparation;
            std::vector<std::vector<PreparedStorage>> replicas;
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
        [[nodiscard]] bool active() const noexcept { return round_.active(); }
        [[nodiscard]] bool canRun() const noexcept;
        [[nodiscard]] FrameContext context() const noexcept { return round_.context(); }
        [[nodiscard]] bool submitting() const noexcept { return active() && submission_->prepared(); }
        [[nodiscard]] bool submitting(const RenderGraphInstance& instance) const noexcept
        {
            return submitting() && submission_->refersTo(instance);
        }
        using UniformStateLookup = cxx::function_ref<const void*(EffectId, graph::NodeId, cxx::basic_type_info) noexcept>;

        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> run(
            std::span<RenderGraphInstance* const>, std::span<const StorageFrameBinding>,
            std::span<const SubmissionOrder> order = {}, std::optional<UniformStateLookup> uniform_state = {}
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
        [[nodiscard]] RenderResult<bool>
        prepareUniforms(std::span<RenderGraphInstance* const>, std::optional<UniformStateLookup>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareStorages(std::span<const StorageFrameBinding>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareInstances(std::span<RenderGraphInstance* const>) noexcept;
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> submitPreparation() noexcept;
        [[nodiscard]] std::uint32_t slot() const noexcept { return round_.context().slot_index; }

        DeviceSession& device_;
        ResourceUploader* uploads_;
        FrameProgress& progress_;
        Workspace& workspace_;
        execution::CpuScheduler cpu_;
        bool parallel_recording_;
        DeviceEntry* owner_;
        FrameProgress::Round round_;
        std::optional<SubmissionOperation> submission_;
        std::optional<RenderError> cancellation_;
        std::size_t prepared_instances_ = 0;
        bool worker_pending_ = false;
        bool recording_pending_ = false;
        bool copies_flushed_ = false;
        bool uniforms_ready_ = false;
        bool resources_ready_ = false;
        bool producer_selections_ready_ = false;
        bool producer_uploads_ready_ = false;
        bool waiting_for_gpu_ = false;
    };
} // namespace lux::render::detail

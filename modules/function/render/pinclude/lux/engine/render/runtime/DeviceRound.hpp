#pragma once

#include <lux/engine/render/runtime/commands/Command.hpp>
#include <exec/async_scope.hpp>
#include <lux/engine/render/resource/ConstantBufferStorage.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/runtime/SubmissionOperation.hpp>
#include <lux/engine/render/runtime/InstalledFrameResource.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/runtime/ProductOrder.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/target/RenderTarget.hpp>
#include <chrono>

namespace lux::render::runtime_detail { struct DeviceRound; }

namespace lux::render::detail
{
    struct RasterDrawInput;
    struct TargetImageSlot;

    // The owner admits compiler work after recordings already using the entire pool have joined.
    class RecordingWorkers final
    {
    public:
        RecordingWorkers(execution::CpuScheduler programs, execution::CpuScheduler recording) noexcept
            : programs_(programs), independent_(recording) {}

        [[nodiscard]] execution::CpuScheduler programScheduler() const noexcept { return programs_; }
        [[nodiscard]] execution::CpuScheduler recordingScheduler() const noexcept { return independent_; }

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
        friend struct runtime_detail::DeviceRound;
        execution::CpuScheduler programs_, independent_;
        exec::async_scope recordings_;
        std::size_t compilations_ = 0;
    };
}

namespace lux::render::runtime_detail
{
    struct JobExecution;
    struct FrameInputs final
    {
        static constexpr std::uint32_t no_job = UINT32_MAX;

        struct TargetRead final
        {
            TargetId target;
            EProductRead read;
            // Installation keeps static entries alive. Dynamic entries are resolved on the Server owner turn.
            // Resize replaces the variant alternative in place, so borrow the stable table entry itself.
            VRenderTarget& entry;
            std::uint32_t writer = no_job;
            detail::OffscreenTarget::SelectedImage selected;

            [[nodiscard]] detail::OffscreenTarget& owner() const noexcept
            {
                return std::get<detail::OffscreenTarget>(entry);
            }
        };

        struct TargetReadSlot final
        {
            std::size_t index;
            bool operator==(const TargetReadSlot&) const noexcept = default;
        };

        struct ImageImport final
        {
            const detail::TargetImageSlot* operand;
            TargetReadSlot source;
            // Dynamic stages cover every raster read of this publication. Static operands use compiled first reads.
            std::uint32_t batch = kGraphInvalidIndex;
            nri::StageBits wait_stages = nri::StageBits::ALL;
        };

        struct Job final
        {
            RenderJobId id;
            std::uint64_t execution_generation;
            // The installed writer holds the Target entry. Resize replaces its content in place.
            VRenderTarget& target;
            std::vector<ImageImport> imports;
            std::size_t static_imports = 0;
            std::span<const ResourceInstanceId> direct_resources, indirect_resources;
            std::vector<std::uint32_t> storages;
            std::vector<std::uint32_t> view_slots;

            [[nodiscard]] bool uses(ResourceInstanceId) const noexcept;
        };

        struct Selection final
        {
            enum class EAvailability { INACTIVE, READY, BLOCKED };
            std::uint32_t output_image = UINT32_MAX;
            EAvailability availability = EAvailability::INACTIVE;
            RenderError error;
            std::uint32_t selected = no_job;
            bool checking_window = false;
            bool window_pinned = false;

            [[nodiscard]] bool active() const noexcept { return availability != EAvailability::INACTIVE; }
            [[nodiscard]] bool ready() const noexcept { return availability == EAvailability::READY; }

            void block(RenderError cause = {}) noexcept
            {
                if (!active()) return;
                availability = EAvailability::BLOCKED;
                if (!error.type) error = cause;
            }
        };

        std::uint64_t revision = 0;
        std::vector<Job> jobs;
        std::vector<TargetRead> sources;
        std::vector<TargetReadSlot> selected_sources;
        void releaseTargetReads() noexcept;

        using TargetSlots = cxx::SlotKeySparseSet<TargetId, std::uint32_t>;
        TargetSlots writers;
        std::array<cxx::SlotKeySparseSet<TargetId, std::size_t>, 2> source_slots;
        std::vector<std::uint32_t> jobs_with_target_lists;
        ProductOrder product_order;
        void orderConsumers() noexcept;
        std::size_t static_sources = 0;
        std::vector<std::uint32_t> binding_ends;
        struct RasterImports final
        {
            std::uint32_t import_index = kGraphInvalidIndex, last_read = kGraphInvalidIndex;
        };
        std::vector<RasterImports> imports_by_source;
        [[nodiscard]] RenderResult<std::uint32_t>
        linkDrawRead(Job&, detail::FrameResources&, ImageImport) noexcept;

        struct ViewSource final
        {
            void* instance;
            decltype(detail::ResourceViewOps::select) select;
        };
        std::vector<ViewSource> views;
        std::vector<detail::InstalledFrameResource> storages;
        // Reserved from the structural bounds and reused for the next device round.
        std::vector<Selection> selection;
        std::vector<std::uint32_t> window_jobs;
        [[nodiscard]] std::uint32_t storageSlot(ResourceInstanceId) const noexcept;
        enum class EResourceUse : std::uint8_t { UNUSED, WINDOW_MEMBER, INPUT_ONLY, GRAPH };
        std::vector<EResourceUse> used_storages;
    };

    struct LUX_RENDER_PUBLIC DeviceRound final
    {
        enum class EStage : std::uint8_t
        {
            IDLE, RESOURCES, FREEZE, FROZEN, SUBMITTING, SETTLED
        };

        DeviceRound(detail::DeviceSession&, detail::FrameProgress&) noexcept;
        ~DeviceRound() noexcept;
        DeviceRound(const DeviceRound&) = delete;
        DeviceRound& operator=(const DeviceRound&) = delete;

        void begin(detail::FrameContext, detail::ResourceUploader*) noexcept;
        [[nodiscard]] RenderResult<void> selectConstants() noexcept;
        [[nodiscard]] bool active() const noexcept { return stage != EStage::IDLE && stage != EStage::SETTLED; }
        // Synchronous owner call. True schedules a staging copy or work on the frozen graph.
        [[nodiscard]] RenderResult<bool> prepareNativeWork() noexcept;
        [[nodiscard]] detail::ReplicaFrame replicaFrame() const noexcept { return {slot(), frame.frame_index + 1}; }
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>>
        submitNativeWork(execution::CpuScheduler, detail::RecordingWorkers* = nullptr) noexcept;
        void cancel(RenderError) noexcept;

        detail::SubmissionOperation::Workspace submissions;
        EStage stage = EStage::IDLE;
        bool pending = false;
        RenderError error;
        FrameInputs inputs;

        struct Output final
        {
            const FrameInputs::Job& job;
            std::uint32_t image_index;
            bool submitted = false;
        };

        struct ResourceUpload final
        {
            std::uint32_t slot;
            detail::ReplicaCopyRange copies{};
            bool ready = false;
        };

        std::vector<Output> outputs;
        std::vector<JobExecution*> executions;
        std::vector<ResourceUpload> resource_uploads;
        std::vector<detail::SubmissionOrder> job_order;

        // The private metrics switch controls sampling; native callers share this object's layout.
        std::chrono::steady_clock::time_point opened_at;

        detail::FrameContext frame{};
        [[nodiscard]] std::uint32_t slot() const noexcept { return frame.slot_index; }

    private:
        [[nodiscard]] RenderResult<bool> stagePreparedBuffers() noexcept;
        [[nodiscard]] RenderResult<void> selectFrameInputs() noexcept;
        [[nodiscard]] RenderResult<bool> stageResourceUploads() noexcept;
        [[nodiscard]] RenderResult<bool> freezeJobs() noexcept;
        void finish() noexcept;
        void settleFrameCopies(detail::FrameCopyReceipt = {}, RenderError = {}) noexcept;

        detail::DeviceSession& device_;
        detail::FrameProgress& progress_;
        detail::ResourceUploader* uploads_ = nullptr;
        std::optional<detail::SubmissionOperation> submission_;
        std::vector<JobExecution*> frozen_jobs_;
        std::vector<detail::RasterDrawInput> freeze_draws_;
        std::size_t attempted_uploads_ = 0;
        std::vector<detail::PreparedBufferInput> buffer_inputs_;
        std::vector<detail::PreparedBufferUpload> buffer_uploads_;
        std::size_t next_upload_ = 0;
        std::vector<RevisionedValue<const void*>> selected_views_;
        detail::SubmissionFrontiers blocked_completion_;
        detail::ConstantBufferSelections buffers_;
        std::vector<detail::DescriptorTable::NativePoolId> held_pools_;
        bool advancing_ = false;
        bool settling_ = false;
        bool waiting_for_gpu_ = false;
    };

} // namespace lux::render::runtime_detail

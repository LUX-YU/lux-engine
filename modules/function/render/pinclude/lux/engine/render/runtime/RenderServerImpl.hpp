#pragma once

#include <lux/engine/render/resource/FrameDataCall.hpp>


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <exec/task.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/runtime/commands/Commands.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/runtime/commands/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/runtime/commands/CommandRegistry.hpp>
#include <lux/engine/render/runtime/RenderJob.hpp>
#include <deque>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/RenderEffect.hpp>
#include <lux/engine/render/runtime/DeviceRound.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/commands/ResponseOutbox.hpp>
#include <lux/engine/render/runtime/Scene.hpp>
#include <lux/engine/render/target/RenderTarget.hpp>

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    struct RenderServer::Impl final
    {
        [[nodiscard]] RenderResult<void> run() noexcept;

        void stop() noexcept
        {
            stop_requested_.store(true);
            ipc::IpcCore::instance().notifyWork();
        }

        [[nodiscard]] RenderServerStats stats() const noexcept;
        [[nodiscard]] std::optional<RenderDevelopmentStats> developmentStats() const noexcept;

        RenderServerConfig config_;
        execution::CpuScheduler cpu_scheduler_; // Borrowed from the application's ExecutionRuntime.
        detail::RecordingWorkers recording_workers_;
        ipc::IpcResponder responder_;
        EffectCatalog effect_catalog_;
        std::atomic<std::uint64_t> validation_errors_{0}, validation_warnings_{0};
        struct RuntimeState final
        {
            ResourceRegistry resources_;
            // Resource scopes consult the logical Jobs during deletion, including device teardown.
            detail::DeviceEntry::JobTable jobs_;
            // Device resource projections borrow stable rows in their logical Scene.
            cxx::StableSlotMap<RenderScene, SceneTag> scenes_;
            cxx::StableSlotMap<std::unique_ptr<detail::DeviceEntry>, DeviceSessionTag> devices_;
            [[nodiscard]] detail::DeviceEntry* device(DeviceSessionId id) const noexcept
            {
                const auto* entry = devices_.find(id);
                return entry ? entry->get() : nullptr;
            }
            CommandRegistry command_registry_;
            cxx::StableSlotMap<VRenderTarget, TargetTag> targets_;
            cxx::StableSlotMap<RenderEffect, EffectTag> effects_;
        };

        // Release the window and actual device before run() returns. Reverse order keeps each borrow valid.
        std::optional<RuntimeState> runtime_{std::in_place};
        std::vector<RenderJobId> running_jobs_;
        const ServerInstanceId instance_id_;

        struct InputWindow final
        {
            ipc::RequesterId requester;
            RequestRenderJob::Run lead;
            std::vector<RequestRenderJob::Run> additional_runs;

            template <class Visitor> void forEachRun(Visitor&& visit) const noexcept
            {
                visit(lead);
                for (const auto& run : additional_runs) visit(run);
            }
        };

        std::vector<InputWindow> input_windows_;
        void releaseInputWindows() noexcept;
        [[nodiscard]] bool waitsForWindow(ipc::RequesterId) const noexcept;
        [[nodiscard]] bool inputQueueFull() const noexcept;
        std::deque<ipc::ReceivedRequest> deferred_inputs_;
        CommandId run_command_;
        CommandId frame_update_command_;
        ResponseOutbox response_outbox_;
        stdexec::counting_scope command_scope_;
        stdexec::simple_counting_scope upload_scope_;
        cxx::execution::ManualContext server_context_;
        std::size_t next_device_ = 0;
        std::uint64_t job_structure_revision_ = 1;
        RenderError execution_error_;
        enum class EState { SETUP, RUNNING, FINISHED };
        std::atomic<EState> state_ = EState::SETUP;
        std::atomic<bool> stop_requested_ = false;

        [[nodiscard]] bool canAccessCatalog() const noexcept
        {
            return state_.load() == EState::SETUP;
        }

        std::atomic<std::uint64_t> ticks_{0}, commands_{0}, compiled_graphs_{0}, submissions_{0};
        std::atomic<std::uint64_t> storages_created_{0};
        std::atomic<std::uint64_t> presentations_{0};
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        std::atomic<std::uint64_t> input_rebuilds_{0}, input_rebuild_ns_{0}, job_selection_ns_{0};
        std::atomic<std::uint64_t> jobs_considered_{0}, jobs_selected_{0}, dependency_edges_{0};
        std::atomic<std::uint64_t> job_selection_calls_{0}, job_selection_growths_{0}, job_topology_growths_{0};
        std::atomic<std::uint64_t> rounds_opened_{0}, slot_cpu_hold_ns_{0};
        std::atomic<std::uint64_t> raster_buffer_ranges_{0}, raster_buffer_bytes_{0}, raster_image_ranges_{0};
        std::atomic<std::uint64_t> raster_image_subresources_{0};
        std::atomic<std::uint64_t> raster_geometry_ranges_{0}, raster_geometry_bytes_{0};
#endif

        Impl(ServerInstanceId, RenderServerConfig, ipc::IpcResponder, execution::ExecutionRuntime&);

        template <RenderCommand C, auto Method> CommandId bindFrameworkCommand() noexcept
        {
            return runtime_->command_registry_.registerCommand(CommandBinding::bind<C, Method>(*this));
        }

        void bindJobCommands() noexcept;
        void createJob(const CreateRenderJob::Request&, JobCreated&) noexcept;
        AckResponse startJob(StartRenderJob::Request&&) noexcept;
        AckResponse rebindJob(RebindRenderJobEffect::Request&&) noexcept;
        void handleStopJob(const RenderJobId&, AckResponse&) noexcept;
        void removeJob(const RenderJobId&, AckResponse&) noexcept;
        void setJobCadence(const SetRenderJobCadence::Request&, AckResponse&) noexcept;
        void requestJob(RequestRenderJob::Request&&, ResponseRoute) noexcept;
        void rejectWindow(RequestRenderJob::Request&, ResponseRoute, RenderError) noexcept;
        void rejectPendingRun(RenderJob&, RenderError) noexcept;
        [[nodiscard]] RenderResult<void> applyWindowInputs(WindowUpdates&, FrameInputs&) noexcept;
        [[nodiscard]] RenderResult<void> applyWindowCommand(WindowCommand&, FrameInputs&) noexcept;
        void stopJob(RenderJob&, RenderError) noexcept;
        void retireStoppedExecution(RenderJob&) noexcept;
        void releaseExecutionResources(
            std::unique_ptr<JobExecution>, std::unique_ptr<detail::JobGpuResources>, std::vector<ResourceInstanceId>&
        ) noexcept;
        void destroyExecution(
            std::unique_ptr<JobExecution>, std::unique_ptr<detail::JobGpuResources>, std::vector<ResourceInstanceId>&
        ) noexcept;
        [[nodiscard]] bool frameUsesResource(const detail::DeviceEntry&, ResourceInstanceId, bool include_frozen) const noexcept;
        [[nodiscard]] bool awaitingInputRead(ResourceInstanceId) const noexcept;
        [[nodiscard]] bool awaitingInputRead(const FrameData&) const noexcept;
        [[nodiscard]] ResourceInstanceId mutableInput(const CommandRecord&, const ipc::IpcMessage&) const noexcept;
        [[nodiscard]] bool awaitingInputRead(CommandId, const ipc::IpcMessage&) const noexcept;
        [[nodiscard]] bool frameUsesTarget(TargetId) const noexcept;
        [[nodiscard]] bool executionTargetsResizing(const JobExecution&) const noexcept;
        [[nodiscard]] bool resourcesAvailable(const FrameInputs::Job&) const noexcept;
        [[nodiscard]] RenderResult<std::vector<RenderJobId>> orderProductConsumers(const JobExecution&) const noexcept;
        [[nodiscard]] bool canChangeExecution(const RenderJob&) const noexcept;

        [[nodiscard]] RenderResult<void>
        assembleExecution(
            JobExecution&, ResourceRegistry::BindingChanges&, const JobBinding&, std::span<const ResolvedResource>
        ) noexcept;

        using ResolvedResources = RenderResult<std::vector<ResolvedResource>>;
        [[nodiscard]] ResolvedResources
        installScopedInputs(JobExecution&, ResourceRegistry::BindingChanges&, ViewScope) noexcept;
        [[nodiscard]] ResolvedResources bindEffectResources(
            JobExecution&, ResourceRegistry::BindingChanges&, ViewScope, std::span<const JobResourceInput>
        ) noexcept;

        enum class EJobStartMode : std::uint8_t { START, REBIND };
        struct JobStartRequest final
        {
            DeviceSessionId device;
            RenderJobId job;
            EffectId effect;
            TargetId target;
            JobBinding binding;
            EJobStartMode mode;
        };

        AckResponse buildJobExecution(JobStartRequest, VRenderTarget* replacement = nullptr) noexcept;

        [[nodiscard]] RenderResult<void>
        validateJobBinding(const JobStartRequest&, const RenderJob&, const compiled::RenderGraph&, const VRenderTarget*) const noexcept;

        using ResizeCandidates = std::vector<std::pair<RenderJobId, detail::ResizeResources>>;
        [[nodiscard]] RenderResult<ResizeCandidates>
        prepareTargetResize(TargetId, detail::TargetOutput, std::span<const RenderJobId>) noexcept;


        void rebuildFrameInputs(detail::DeviceEntry&) noexcept;
        [[nodiscard]] std::optional<FrameInputs::TargetReadSlot> resolveTargetRead(FrameInputs&, RasterTargetImage) noexcept;
        [[nodiscard]] RenderResult<bool> appendDrawReads(FrameInputs&, std::uint32_t reader, std::uint32_t slot) noexcept;
        void selectFrameInputs(detail::DeviceEntry&, detail::FrameContext) noexcept;
        void qualifyTargetReads(detail::DeviceEntry&, std::uint32_t job, bool collect_producers = false) noexcept;
        [[nodiscard]] RenderResult<bool> qualifyWindowTargets(detail::DeviceEntry&) noexcept;
        void adoptReadyWindows(detail::DeviceEntry&, detail::FrameContext) noexcept;
        void bindSelectedTargets(detail::DeviceEntry&, std::uint32_t slot) noexcept;
        void progressTargetFlights(detail::DeviceEntry&) noexcept;
        [[nodiscard]] std::vector<RenderJobId> jobsBindingTarget(TargetId) const noexcept;
        exec::task<RenderResult<void>> submitCurrentFrame(detail::DeviceEntry&) noexcept;
        void cancelActiveFrame(detail::DeviceEntry&, RenderError) noexcept;
        [[nodiscard]] bool serviceOperations(bool admitting) noexcept;
        void waitForService(std::uint64_t observed) const noexcept;
        void progressUploads(detail::DeviceEntry&) noexcept;
        [[nodiscard]] bool progressFrame(detail::DeviceEntry&) noexcept;

        [[nodiscard]] detail::DeviceEntry* resourceDevice(ResourceInstanceId) const noexcept;
        UpdateFrameData::Response updateFrameData(UpdateFrameData::Request&&) noexcept;
        std::vector<detail::FrameDataCall> selected_updates_;
        [[nodiscard]] RenderResult<void>
        adoptFrameData(const FrameData&, FrameInputs* = nullptr) noexcept;
        void finishFrame(detail::DeviceEntry&, RenderError) noexcept;
        [[nodiscard]] bool beginFrame(detail::DeviceEntry&) noexcept;
        void submitFrameRendering(detail::DeviceEntry&) noexcept;

        static void NRI_CALL
        diagnostic(nri::Message severity, const char*, std::uint32_t, const char*, void* user) noexcept;

        void queryResource(const ResourceRequest&, ResourceResponse&) noexcept;
        void listResourceJobs(const ResourceAddress&, ListResourceJobs::Response&) noexcept;
        void removeResource(const ResourceAddress&, AckResponse&) noexcept;
        void getOrCreateResource(const ResourceRequest&, ResourceResponse&) noexcept;
        [[nodiscard]] RenderResult<ResourceSet*>
        jobResourceScope(detail::DeviceEntry&, JobExecution&, ViewScope, ResourceCreateContext*) noexcept;
        [[nodiscard]] RenderResult<ResourceSet*>
        resourceScope(detail::DeviceEntry&, const VResourceScope&, ResourceTypeId, ResourceCreateContext*) noexcept;

        void createScene(const CreateScene::Request&, CreateScene::Response&) noexcept;

        void removeScene(const RemoveScene::Request&, RemoveScene::Response&) noexcept;

        void createView(const CreateView::Request&, CreateView::Response&) noexcept;
        void updateView(const UpdateView::Request&, UpdateView::Response&) noexcept;
        void updateJobInputs(const UpdateJobInputs::Request&, AckResponse&) noexcept;
        void invalidateHistory(const InvalidateHistory::Request&, InvalidateHistory::Response&) noexcept;

        void removeView(const RemoveView::Request&, RemoveView::Response&) noexcept;

        void createTarget(const CreateTarget::Request&, CreateTarget::Response&) noexcept;

        exec::task<TargetCreated> resizeTarget(ResizeTarget::Request&&) noexcept;
        exec::task<AckResponse> removeTarget(RemoveTarget::Request&&) noexcept;
        void setTargetActive(const SetTargetActive::Request&, SetTargetActive::Response&) noexcept;

        exec::task<AckResponse> prepareEffect(PrepareEffect::Request&&) noexcept;
        void listEffectJobs(const EffectRequest&, ListEffectJobs::Response&) noexcept;
        EffectCreated createEffect(CreateEffect::Request&&) noexcept;
        void bindEffectGraphCommands() noexcept;
        void removeEffect(const RemoveEffect::Request&, RemoveEffect::Response&) noexcept;

        void queryRenderStats(const QueryRenderStats::Request&, QueryRenderStats::Response&) noexcept;
        void createDeviceSession(const CreateDeviceSession::Request&, CreateDeviceSession::Response&) noexcept;
        exec::task<AckResponse> removeDeviceSession(RemoveDeviceSession::Request&&) noexcept;
        void queryRenderDevice(const QueryRenderDevice::Request&, QueryRenderDevice::Response&) noexcept;
        void queryExecutionProgress(const QueryExecutionProgress::Request&, QueryExecutionProgress::Response&) noexcept;

        void listEffectCapabilities(const EmptyRequest&, ListEffectCapabilities::Response&) noexcept;
        void listCommands(const ListCommands::Request&, ListCommands::Response&) noexcept;
        void queryCommandIds(const QueryCommandIds::Request&, QueryCommandIds::Response&) noexcept;

        void stopCommand(const Stop::Request&, Stop::Response&) noexcept;

        void receiveRequest(ipc::ReceivedRequest) noexcept;
        void dispatchRequest(ipc::ReceivedRequest) noexcept;
        void rejectInput(ipc::ReceivedRequest&, RenderError) noexcept;
        bool drainInputs() noexcept;
        [[nodiscard]] bool hasDeferredRun(RenderJobId) const noexcept;
        void cancelDeferredRuns(RenderJobId) noexcept;
        [[nodiscard]] RenderResult<HandlerCall>
        resolveCommandCall(CommandId, const CommandRecord&, runtime_detail::CommandDispatchContext&) noexcept;
        void failDevice(detail::DeviceEntry&, RenderError) noexcept;
        void failExecution(RenderError error) noexcept;

        std::size_t publishResponses(std::size_t limit) noexcept;

        void shutdown() noexcept;
    };
} // namespace lux::render

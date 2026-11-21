#pragma once

#include <lux/engine/render/runtime/JobOutput.hpp>

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
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/commands/CommandRegistry.hpp>
#include <lux/engine/render/runtime/RenderJob.hpp>
#include <deque>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/RenderEffect.hpp>
#include <lux/engine/render/runtime/DeviceRound.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/commands/ResponseOutbox.hpp>
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
        execution::CpuScheduler program_scheduler_, recording_scheduler_;
        ipc::IpcResponder responder_;
        EffectCatalog effect_catalog_;
        std::atomic<std::uint64_t> validation_errors_{0}, validation_warnings_{0};
        struct RuntimeState final
        {
            ResourceRegistry resources_;
            cxx::StableSlotMap<std::unique_ptr<detail::DeviceEntry>, DeviceSessionTag> devices_;
            [[nodiscard]] detail::DeviceEntry* device(DeviceSessionId id) const noexcept
            {
                const auto* entry = devices_.find(id);
                return entry ? entry->get() : nullptr;
            }
            CommandRegistry command_registry_;
            cxx::StableSlotMap<RenderScene, SceneTag> scenes_;
            cxx::StableSlotMap<VRenderTarget, TargetTag> targets_;
            cxx::StableSlotMap<RenderEffect, EffectTag> effects_;
            cxx::StableSlotMap<RenderJob, RenderJobTag> jobs_;
        };

        // Release the window and actual device before run() returns. Reverse order keeps each borrow valid.
        std::optional<RuntimeState> runtime_{std::in_place};
        std::vector<RenderJobId> running_jobs_;
        const ServerInstanceId instance_id_;
        std::uint64_t next_input_layout_revision_ = 1;

        ResponseOutbox response_outbox_;
        stdexec::counting_scope command_scope_;
        stdexec::simple_counting_scope upload_scope_;
        cxx::execution::ManualContext server_context_;
        // One dynamic program preparation may be away from the owner at a time.
        JobExecution* preparing_execution_ = nullptr;
        bool program_waiting_for_recording_ = false;
        std::vector<TargetId> preparation_targets_;
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
        std::atomic<std::uint64_t> program_preparation_ns_{0}, program_preparations_{0}, discarded_preparations_{0};
        std::atomic<std::uint64_t> raster_buffer_ranges_{0}, raster_buffer_bytes_{0}, raster_image_ranges_{0};
        std::atomic<std::uint64_t> raster_image_subresources_{0};
        std::atomic<std::uint64_t> raster_geometry_ranges_{0}, raster_geometry_bytes_{0};
        std::chrono::steady_clock::time_point program_preparation_started_at_;
#endif

        Impl(ServerInstanceId, RenderServerConfig, ipc::IpcResponder, execution::ExecutionRuntime&);

        template <RenderCommand C, auto Method> CommandId bindFrameworkCommand() noexcept
        {
            return runtime_->command_registry_.registerCommand(CommandBinding::bind<C, Method>(*this));
        }

        void bindJobCommands() noexcept;
        void createJob(const CreateRenderJob::Request&, JobCreated&) noexcept;
        exec::task<AckResponse> startJob(StartRenderJob::Request&&) noexcept;
        exec::task<AckResponse> rebindJob(RebindRenderJobEffect::Request&&) noexcept;
        void handleStopJob(const RenderJobId&, AckResponse&) noexcept;
        void removeJob(const RenderJobId&, AckResponse&) noexcept;
        void setJobCadence(const SetRenderJobCadence::Request&, AckResponse&) noexcept;
        void setJobDependencies(const SetRenderJobDependencies::Request&, AckResponse&) noexcept;
        auto requestJob(RequestRenderJob::Request&&) noexcept;
        void stopJob(RenderJob&) noexcept;
        void retireStoppedExecution(RenderJob&) noexcept;
        void destroyExecution(std::unique_ptr<JobExecution>) noexcept;
        [[nodiscard]] RenderResult<void> retainExecutionResource(JobExecution&, ResourceInstanceId) noexcept;
        [[nodiscard]] bool jobTargetReady(const RenderJob&) const noexcept;
        [[nodiscard]] bool frameUsesResource(const detail::DeviceEntry&, ResourceInstanceId) const noexcept;
        [[nodiscard]] bool frameUsesTarget(TargetId) const noexcept;
        [[nodiscard]] bool preparationUsesTarget(TargetId) const noexcept;
        [[nodiscard]] bool resourcesAvailable(std::span<const ResourceInstanceId>) const noexcept;
        [[nodiscard]] std::vector<detail::JobRelationEntry> jobRelations() noexcept;
        [[nodiscard]] bool canChangeExecution(const RenderJob&) const noexcept;

        [[nodiscard]] RenderResult<void> bindGraphSamplers(JobExecution&, std::span<const SamplerDescription>) noexcept;

        [[nodiscard]] RenderResult<void> assembleExecution(JobExecution&, const JobBinding&) noexcept;

        [[nodiscard]] RenderResult<std::vector<EffectTargetImport>>
        qualifyEffectTargetImports(std::span<const TargetImageInput>) const noexcept;

        [[nodiscard]] RenderResult<EffectImportContracts>
        qualifyEffectImports(const detail::DeviceEntry&, const JobBinding&) const noexcept;

        exec::task<detail::EffectVariantResult> acquireEffectVariant(
            EffectId, RenderEffect&, detail::DeviceEntry&, EffectOutputDesc, const EffectImportContracts&
        ) noexcept;

        exec::task<RenderResult<std::unique_ptr<const JobGraphPlan>>> compileEffectVariant(
            EffectId, RenderEffect&, detail::DeviceEntry&, const EffectOutputDesc&, const EffectImportContracts&
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

        exec::task<AckResponse> buildJobExecution(JobStartRequest) noexcept;
        [[nodiscard]] RenderResult<void> buildJobInputLayout(const RenderJob&, JobExecution&) noexcept;

        [[nodiscard]] RenderResult<void> validateJobBinding(
            DeviceSessionId,
            const RenderJob&, const RenderEffect&, TargetId, const JobBinding&, detail::TargetReservation
        ) const noexcept;

        using ResizeCandidates = std::vector<std::pair<RenderJobId, detail::RenderGraphInstance::ResizeResources>>;
        [[nodiscard]] RenderResult<ResizeCandidates>
        prepareTargetResize(TargetId, detail::TargetOutput, std::span<const RenderJobId>) noexcept;

        [[nodiscard]] bool
        targetDependencyCycle(TargetId, std::span<const TargetImageInput>, RenderJobId) const noexcept;

        void rebuildFrameInputs(detail::DeviceEntry&) noexcept;
        [[nodiscard]] RenderError selectFrameInputs(detail::DeviceEntry&, std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> selectGraphInputs(detail::DeviceEntry&, std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<bool> acquireFrameOutputs(detail::DeviceEntry&, std::uint32_t slot) noexcept;
        void progressTargetFlights(detail::DeviceEntry&) noexcept;
        [[nodiscard]] std::vector<RenderJobId> jobsSamplingTarget(TargetId) const noexcept;
        [[nodiscard]] std::uint32_t publishedOutputSlot(TargetId) const noexcept;
        [[nodiscard]] bool publishedOutputReadable(TargetId, std::uint32_t slot) const noexcept;
        exec::task<RenderResult<void>> submitCurrentFrame(detail::DeviceEntry&) noexcept;
        void cancelActiveFrame(detail::DeviceEntry&, RenderError) noexcept;
        [[nodiscard]] bool serviceOperations(bool admitting) noexcept;
        void waitForService(std::uint64_t observed) const noexcept;
        void progressUploads(detail::DeviceEntry&) noexcept;
        [[nodiscard]] bool progressFrame(detail::DeviceEntry&) noexcept;

        [[nodiscard]] bool progressProgramPreparations(detail::DeviceEntry&) noexcept;
        void startProgramPreparation(RenderJob&, JobExecution&) noexcept;
        void queryFrameInputLayout(const RenderJobId&, QueryRenderJobInputLayout::Response&) noexcept;
        void updateFrameData(const UpdateFrameData::Request&, AckResponse&) noexcept;
        [[nodiscard]] RenderResult<void> adoptFrameData(const JobExecution&, const FrameData&) noexcept;
        void finishFrame(detail::DeviceEntry&, RenderError) noexcept;
        [[nodiscard]] bool beginFrame(detail::DeviceEntry&) noexcept;
        void submitFrameRendering(detail::DeviceEntry&) noexcept;

        static void NRI_CALL
        diagnostic(nri::Message severity, const char*, std::uint32_t, const char*, void* user) noexcept;

        void queryResource(const ResourceRequest&, ResourceResponse&) noexcept;
        void getOrCreateResource(const ResourceRequest&, ResourceResponse&) noexcept;
        [[nodiscard]] RenderResult<ResourceSet*>
        resourceScope(detail::DeviceEntry&, const VRenderScope&, ResourceTypeId, bool create) noexcept;

        void createScene(const CreateScene::Request&, CreateScene::Response&) noexcept;

        void removeScene(const RemoveScene::Request&, RemoveScene::Response&) noexcept;

        void createView(const CreateView::Request&, CreateView::Response&) noexcept;
        void updateJobInputs(const UpdateJobInputs::Request&, AckResponse&) noexcept;
        void invalidateHistory(const InvalidateHistory::Request&, InvalidateHistory::Response&) noexcept;

        void removeView(const RemoveView::Request&, RemoveView::Response&) noexcept;

        void createTarget(const CreateTarget::Request&, CreateTarget::Response&) noexcept;

        exec::task<TargetCreated> resizeTarget(ResizeTarget::Request&&) noexcept;
        exec::task<AckResponse> removeTarget(RemoveTarget::Request&&) noexcept;
        void setTargetActive(const SetTargetActive::Request&, SetTargetActive::Response&) noexcept;

        auto createEffect(CreateEffect::Request&&) noexcept;
        void bindCreateEffectCommand() noexcept;
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

        void dispatchRequest(ipc::ReceivedRequest received) noexcept;
        [[nodiscard]] RenderResult<HandlerCall>
        resolveCommandCall(CommandId, const CommandRecord&, runtime_detail::CommandDispatchContext&) noexcept;
        void failDevice(detail::DeviceEntry&, RenderError) noexcept;
        void failExecution(RenderError error) noexcept;

        std::size_t publishResponses(std::size_t limit) noexcept;

        void shutdown() noexcept;
    };
} // namespace lux::render

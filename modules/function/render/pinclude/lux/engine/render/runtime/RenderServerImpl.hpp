#pragma once

#include <lux/engine/render/runtime/JobOutput.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
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
#include <lux/engine/render/capabilities/CapabilityRegistry.hpp>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/commands/CommandRegistry.hpp>
#include <lux/engine/render/runtime/RenderJob.hpp>
#include <deque>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/EffectBuild.hpp>
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
        [[nodiscard]] RenderResult<void> initialize(DeviceOptions options) noexcept;
        FeatureId publishFeature(FeatureEntry) noexcept;
        RenderResult<void>
        validateImplementationDescription(const CapabilityDescription&, const CapabilityImplementation&) const noexcept;
        RenderResult<void>
        validateCapabilityImplementation(const CapabilityDescription&, const CapabilityImplementation&) const noexcept;
        RenderResult<void> validateFeature(const FeatureEntry&) const noexcept;
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
        ipc::IpcResponder responder_;
        cxx::StableSlotMap<FeatureEntry, FeatureTag> features_;
        std::map<std::string_view, FeatureId, std::less<>> features_by_name_;
        CapabilityRegistry capability_registry_;
        std::atomic<std::uint64_t> validation_errors_{0}, validation_warnings_{0};
        struct DeviceDomain final
        {
            ResourceRegistry resources_;
            std::unique_ptr<detail::DeviceEntry> device_;
            CommandRegistry command_registry_;
            cxx::StableSlotMap<RenderScene, SceneTag> scenes_;
            cxx::StableSlotMap<VRenderTarget, TargetTag> targets_;
            cxx::SlotKeySparseSet<TargetId, detail::JobOutput> job_outputs_;
            cxx::StableSlotMap<RenderEffect, EffectTag> effects_;
            cxx::StableSlotMap<RenderJob, RenderJobTag> jobs_;
        };

        // Release the window and actual device before run() returns. Reverse order keeps each borrow valid.
        std::optional<DeviceDomain> domain_{std::in_place};
        std::vector<RenderJobId> running_jobs_;
        std::uint64_t next_input_layout_revision_ = 1;

        ResponseOutbox response_outbox_;
        stdexec::counting_scope command_scope_;
        stdexec::simple_counting_scope upload_scope_;
        bool upload_progress_pending_ = false;
        cxx::execution::ManualContext server_context_;
        DeviceRound round_;
        std::deque<InputUpdate*> input_updates_;
        std::vector<detail::RenderGraphInstance*> frame_instances_;
        struct FrameOutput final
        {
            RenderJobId job;
            JobExecution* execution;
            TargetId target;
            std::vector<TargetId> reads;
            bool submitted = false;
        };

        std::vector<FrameOutput> frame_outputs_;
        std::vector<RenderJobId> unready_jobs_;
        std::vector<RenderJobId> target_readiness_scratch_;
        std::deque<detail::JobControl::Run> frame_runs_;
        std::size_t frame_run_count_ = 0;
        detail::SubmissionFrontiers target_frame_uses_;
        std::vector<detail::StorageFrameUse> frame_storages_;
        std::vector<detail::SubmissionOrder> frame_job_order_;
        std::uint64_t output_uses_revision_ = 0;
        RenderError frame_input_error_;
        RenderError execution_error_;
        bool active_frame_inputs_dirty_ = true;
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
        std::atomic<std::uint64_t> rounds_opened_{0}, slot_cpu_hold_ns_{0};
        std::atomic<std::uint64_t> program_preparations_holding_slot_{0}, program_preparation_ns_{0};
        std::chrono::steady_clock::time_point slot_opened_at_, program_preparation_started_at_;
#endif

        Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler cpu_scheduler);

        template <RenderCommand C, auto Method> CommandId bindFrameworkCommand() noexcept
        {
            return domain_->command_registry_.registerCommand(CommandBinding::bind<C, Method>(*this));
        }

        void bindJobCommands() noexcept;
        void createJob(const CreateRenderJob::Request&, JobCreated&) noexcept;
        exec::task<AckResponse> startJob(StartRenderJob::Request&&) noexcept;
        exec::task<AckResponse> rebindJob(RebindRenderJobEffect::Request&&) noexcept;
        void handleStopJob(const RenderJobId&, AckResponse&) noexcept;
        void removeJob(const RenderJobId&, AckResponse&) noexcept;
        void setJobCadence(const SetRenderJobCadence::Request&, AckResponse&) noexcept;
        void setJobDependencies(const SetRenderJobDependencies::Request&, AckResponse&) noexcept;
        exec::task<JobSubmitted> requestJob(RequestRenderJob::Request&&) noexcept;
        void stopJob(RenderJob&) noexcept;
        void retireStoppedExecution(RenderJob&) noexcept;
        [[nodiscard]] bool jobTargetReady(const RenderJob&) const noexcept;
        [[nodiscard]] bool frameUsesResource(ResourceInstanceId) const noexcept;
        [[nodiscard]] bool frameUsesTarget(TargetId) const noexcept;
        [[nodiscard]] bool resourcesAvailable(std::span<const ResourceInstanceId>) const noexcept;
        [[nodiscard]] std::vector<detail::JobRelationEntry> jobRelations() noexcept;
        [[nodiscard]] bool canChangeExecution(const RenderJob&) const noexcept;

        [[nodiscard]] RenderResult<std::vector<ResourceInstanceId>>
        resolveJobResources(const RenderEffect&, RenderView&) noexcept;

        [[nodiscard]] RenderResult<void> assembleExecution(
            JobExecution&, RenderView&, std::span<const ResourceInstanceId>, const JobBinding&
        ) noexcept;

        exec::task<AckResponse>
        buildJobExecution(RenderJobId, EffectId, TargetId, JobBinding, bool rebind) noexcept;

        [[nodiscard]] RenderResult<void> validateJobBinding(
            const RenderJob&, const RenderEffect&, TargetId, const JobBinding&, const void* reservation
        ) const noexcept;

        using ResizeCandidates = std::vector<std::pair<RenderJobId, detail::RenderGraphInstance::ResizeResources>>;
        [[nodiscard]] RenderResult<ResizeCandidates>
        prepareTargetResize(TargetId, detail::TargetOutput, std::span<const RenderJobId>) noexcept;

        [[nodiscard]] bool
        targetDependencyCycle(TargetId, std::span<const TargetImageInput>, RenderJobId) const noexcept;

        void rebuildActiveFrameInputs() noexcept;
        [[nodiscard]] RenderResult<void> applyParameterSources(std::uint32_t slot) noexcept;
        void prefetchFrameOutputs(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<bool> acquireFrameOutputs(std::uint32_t slot) noexcept;
        void progressTargetFlights() noexcept;
        [[nodiscard]] std::vector<RenderJobId> jobsSamplingTarget(TargetId) const noexcept;
        [[nodiscard]] std::uint32_t outputSlot(TargetId, std::uint32_t frame_slot) const noexcept;
        exec::task<RenderResult<void>> submitCurrentFrame() noexcept;
        void cancelActiveFrame(RenderError) noexcept;
        [[nodiscard]] bool serviceOperations(bool admitting) noexcept;
        void waitForService(std::uint64_t observed) const noexcept;
        void progressUploads() noexcept;
        [[nodiscard]] bool progressFrame() noexcept;
        [[nodiscard]] bool progressInputUpdates() noexcept;
        void queryFrameInputLayout(const RenderJobId&, QueryRenderJobInputLayout::Response&) noexcept;
        exec::task<AckResponse> updateFrameData(UpdateFrameData::Request&&) noexcept;
        [[nodiscard]] RenderResult<void> applyFrameData(const JobExecution&, const FrameData&) noexcept;
        void setEffectParameters(const SetEffectParameters::Request&, AckResponse&) noexcept;
        void updateViewCamera(const UpdateViewCamera::Request&, AckResponse&) noexcept;
        void finishFrame(RenderError) noexcept;
        [[nodiscard]] bool beginFrame() noexcept;
        void submitFrameRendering() noexcept;
        void prepareFramePrograms() noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> prepareInstancePrograms() noexcept;

        static void NRI_CALL
        diagnostic(nri::Message severity, const char*, std::uint32_t, const char*, void* user) noexcept;

        void queryResource(const ResourceRequest&, ResourceResponse&) noexcept;
        void getOrCreateResource(const ResourceRequest&, ResourceResponse&) noexcept;
        [[nodiscard]] RenderResult<ResourceSet*> resourceScope(const VRenderScope&, ResourceTypeId) noexcept;

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

        using EffectBuildResult = RenderResult<RenderEffect>;
        [[nodiscard]] exec::task<EffectCreated> adoptBuiltEffect(EffectBuildResult) noexcept;
        auto createEffect(CreateEffect::Request&&) noexcept;
        [[nodiscard]] RenderResult<runtime_detail::QualifiedEffectRecipe>
        qualifyEffectRecipe(const CreateEffectRequest&) const noexcept;
        void bindCreateEffectCommand() noexcept;

        [[nodiscard]] static RenderError
        capabilityOutputError(const CapabilityImplementation&, const EffectOutputDesc&) noexcept;

        void listCapabilities(const ListCapabilities::Request&, ListCapabilities::Response&) noexcept;


        [[nodiscard]] RenderResult<ResourceSet*> findScope(const VRenderScope&) noexcept;

        void removeEffect(const RemoveEffect::Request&, RemoveEffect::Response&) noexcept;

        void queryRenderStats(const QueryRenderStats::Request&, QueryRenderStats::Response&) noexcept;
        void queryRenderDevice(const QueryRenderDevice::Request&, QueryRenderDevice::Response&) noexcept;
        void queryExecutionProgress(const QueryExecutionProgress::Request&, QueryExecutionProgress::Response&) noexcept;

        void listCommands(const ListCommands::Request&, ListCommands::Response&) noexcept;
        void queryCommandIds(const QueryCommandIds::Request&, QueryCommandIds::Response&) noexcept;

        void stopCommand(const Stop::Request&, Stop::Response&) noexcept;

        void dispatchRequest(ipc::ReceivedRequest received) noexcept;
        [[nodiscard]] RenderResult<HandlerCall>
        resolveCommandCall(CommandId, const CommandRecord&, const ipc::IpcMessage&, ResourceCommandUse& resource) noexcept;
        void failExecution(RenderError error) noexcept;

        std::size_t publishResponses(std::size_t limit) noexcept;

        void shutdown() noexcept;
    };
} // namespace lux::render

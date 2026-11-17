#pragma once

#include <thread>

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
#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/commands/CommandRegistry.hpp>
#include <lux/engine/render/runtime/EffectBuild.hpp>
#include <lux/engine/render/runtime/FrameSubmission.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/commands/ResponseOutbox.hpp>
#include <lux/engine/render/runtime/Scene.hpp>
#include <thread>

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    struct RenderServer::Impl final
    {
        [[nodiscard]] RenderResult<void> initialize(DeviceOptions options) noexcept;
        [[nodiscard]] RenderResult<void> registerBuiltinFeatures() noexcept;
        FeatureId publishFeature(FeatureEntry) noexcept;
        RenderResult<void>
        validateImplementationDescription(const CapabilityDescription&, const CapabilityImplementation&) const noexcept;
        RenderResult<void>
        validateCapabilityImplementation(const CapabilityDescription&, const CapabilityImplementation&) const noexcept;
        RenderResult<void> validateFeature(const FeatureEntry&) const noexcept;
        RenderResult<void> validateRegistration(
            std::span<const ResourceRegistration>,
            std::span<const HandlerRecord>,
            std::span<const CommandBinding> = {}
        ) const noexcept;
        void publishRegistration(
            std::span<const ResourceRegistration>,
            std::span<HandlerRecord>,
            std::span<const CommandBinding>
        ) noexcept;

        [[nodiscard]] RenderResult<void> run() noexcept;

        void stop() noexcept
        {
            stop_requested_.store(true);
            ipc::IpcCore::instance().notifyWork();
        }

        [[nodiscard]] RenderServerStats stats() const noexcept;

        RenderServerConfig config_;
        execution::CpuScheduler cpu_scheduler_; // Borrowed from the application's ExecutionRuntime.
        ipc::IpcResponder responder_;
        cxx::StableSlotMap<FeatureEntry, FeatureTag> features_;
        std::map<std::string_view, FeatureId, std::less<>> features_by_name_;
        CommandRegistry command_registry_;
        CapabilityRegistry capability_registry_;
        cxx::StableSlotMap<RenderScene, SceneTag> scenes_;
        cxx::StableSlotMap<RenderTarget, TargetTag> targets_;
        cxx::StableSlotMap<RenderEffect, EffectTag> effects_;
        cxx::SlotMap<EffectAttachment, AttachmentTag> attachments_;
        cxx::SlotKeySparseSet<TargetId, std::monostate> active_targets_;
        std::unique_ptr<detail::DeviceSession> device_;
        std::unique_ptr<detail::ResourceUploader> uploads_;
        std::unique_ptr<detail::FrameExecution> frames_;
        std::optional<ResourceRegistry> resources_;
        std::optional<ResourceSet> global_storages_;
        ipc::ReceivedRequest deferred_request_;
        bool structural_command_running_ = false;
        bool paged_instances_ = false;

        [[nodiscard]] ResourceCreateContext resourceCreateContext() noexcept
        {
            return {*device_, uploads_.get(), config_.frames_in_flight, *resources_, paged_instances_};
        }

        ResponseOutbox response_outbox_;
        stdexec::counting_scope command_scope_;
        stdexec::simple_counting_scope upload_scope_;
        bool upload_progress_pending_ = false;
        cxx::execution::ManualContext server_context_;
        ipc::RequesterId frame_source_;
        CommandId submit_frame_command_, submit_ui_frame_command_;
        CommandId select_binding_command_;
        std::vector<FrameRecord> frame_inputs_;
        std::size_t first_frame_ = 0;
        std::size_t frame_count_ = 0;
        bool frame_resynchronization_ = false;
        std::vector<detail::RenderGraphInstance*> frame_instances_;
        std::vector<detail::SurfaceTarget*> frame_surfaces_;
        std::vector<detail::StorageFrameUse> frame_storages_;
        std::vector<std::pair<detail::RenderGraphInstance*, detail::FeatureBinding*>> frame_features_;
        bool active_frame_inputs_dirty_ = true;
        std::atomic<bool> stop_requested_{false}, has_started_{false}, running_{false};
        // Startup is single-threaded. Running catalog access is confined to the actual drive stack.
        const std::thread::id setup_thread_ = std::this_thread::get_id();
        static thread_local Impl* catalog_owner_;
        std::atomic<bool> run_finished_{false};

        [[nodiscard]] bool canAccessCatalog() const noexcept
        {
            if (catalog_owner_ == this) return frame_count_ == 0 && !structural_command_running_;
            const bool is_setup_thread = setup_thread_ == std::this_thread::get_id();
            return is_setup_thread && (!has_started_.load() || run_finished_.load());
        }

        std::atomic<std::uint64_t> ticks_{0}, commands_{0}, compiled_graphs_{0}, submissions_{0};
        std::atomic<std::uint64_t> validation_errors_{0}, validation_warnings_{0};
        std::atomic<std::uint64_t> storages_created_{0};
        std::atomic<std::uint64_t> presentations_{0};

        Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler cpu_scheduler);

        template <RenderCommand C, auto Method> CommandId bindFrameworkCommand() noexcept
        {
            return command_registry_.registerCommand(CommandBinding::bind<C, Method>(*this));
        }

        void detachAttachment(AttachmentId id) noexcept;
        [[nodiscard]] bool canChangeAttachments(std::span<const AttachmentId>) const noexcept;

        RenderResult<runtime_detail::EffectAttachment> prepareAttachment(
            const runtime_detail::RenderEffect&,
            std::unique_ptr<detail::RenderPlan>,
            detail::TargetOutput,
            ResourceSet& scene,
            runtime_detail::RenderView& view,
            const AttachmentRequest& input
        ) noexcept;

        [[nodiscard]] RenderResult<void> validateAttachment(const AttachmentRequest&) const noexcept;
        exec::task<AttachmentCreated> attachEffect(AttachmentRequest&&) noexcept;

        using ResizeCandidates = std::vector<std::pair<AttachmentId, detail::RenderGraphInstance::ResizeResources>>;
        [[nodiscard]] RenderResult<ResizeCandidates>
            prepareTargetResize(TargetId, detail::TargetOutput, std::span<const AttachmentId>) noexcept;

        [[nodiscard]] bool
            targetDependencyCycle(TargetId, std::span<const TargetImageInput>, AttachmentId) const noexcept;

        void rebuildActiveFrameInputs() noexcept;
        [[nodiscard]] RenderResult<void> applyViewCameras() noexcept;
        [[nodiscard]] RenderResult<bool> acquireFrameOutputs(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> presentFrameOutputs() noexcept;
        exec::task<RenderResult<void>> submitCurrentFrame(std::uint32_t slot) noexcept;
        RenderResult<void> progressGpuCompletions() noexcept;
        void progressUploads() noexcept;
        void progressFrame() noexcept;

        auto submitFrame(SubmitFrame::Request&&) noexcept;
        auto submitUiFrame(SubmitUiFrame::Request&&) noexcept;
        void queryFrameInputLayout(const AttachmentId&, QueryFrameInputLayout::Response&) noexcept;
        void updateViewCamera(const UpdateViewCamera::Request&, AckResponse&) noexcept;
        void bindSubmitFrameCommand() noexcept;
        void admitFrame(
            RenderFrame, void*, void (*)(void*, FrameCompletion) noexcept, std::span<const UiDrawInput> = {}
        ) noexcept;
        void finishFrame(FrameRecord&, FrameCompletion) noexcept;
        void activateFrame(FrameRecord&) noexcept;
        void applyFrameInputs(FrameRecord&) noexcept;
        void failFrame(FrameRecord&) noexcept;
        void submitFrameRendering(FrameRecord&) noexcept;
        void finishFrameRendering(FrameRecord&, RenderResult<void>) noexcept;
        void prepareFramePrograms(FrameRecord&) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> prepareRasterPrograms() noexcept;

        static void NRI_CALL
        diagnostic(nri::Message severity, const char*, std::uint32_t, const char*, void* user) noexcept;

        void queryResource(const ResourceRequest&, ResourceResponse&) noexcept;
        void getOrCreateResource(const ResourceRequest&, ResourceResponse&) noexcept;
        [[nodiscard]] RenderResult<ResourceSet*> resourceScope(const VRenderScope&, ResourceTypeId) noexcept;

        void createScene(const CreateScene::Request&, CreateScene::Response&) noexcept;

        void removeScene(const RemoveScene::Request&, RemoveScene::Response&) noexcept;

        void createView(const CreateView::Request&, CreateView::Response&) noexcept;
        void updateAttachmentInputs(const UpdateAttachmentInputs::Request&, UpdateAttachmentInputs::Response&) noexcept;
        void invalidateHistory(const InvalidateHistory::Request&, InvalidateHistory::Response&) noexcept;

        void removeView(const RemoveView::Request&, RemoveView::Response&) noexcept;

        void createTarget(const CreateTarget::Request&, CreateTarget::Response&) noexcept;

        exec::task<TargetCreated> resizeTarget(ResizeTarget::Request&&) noexcept;
        exec::task<AckResponse> removeTarget(RemoveTarget::Request&&) noexcept;
        void setTargetActive(const SetTargetActive::Request&, SetTargetActive::Response&) noexcept;

        using EffectBuildResult = RenderResult<RenderEffect>;
        [[nodiscard]] exec::task<EffectCreated> adoptBuiltEffect(
            EffectBuildResult,
            std::vector<RenderEffect::Implementation>
        ) noexcept;

        auto buildGraphEffect(
            GraphEffectRequest&&,
            RenderError = {},
            std::vector<RenderEffect::Implementation> = {},
            std::vector<RenderEffect::ResourceDependency> = {}
        ) noexcept;
        auto createCapabilityEffect(CreateEffect::Request&&) noexcept;
        [[nodiscard]] RenderResult<void>
        buildCapabilityGraph(
            CreateEffectRequest&,
            GraphEffectRequest&,
            std::vector<RenderEffect::ResourceDependency>&
        ) noexcept;
        [[nodiscard]] RenderResult<const ConfigValue*>
        qualifyConfiguration(const VCapabilityConfiguration&, const CapabilityImplementation&, ConfigValue& decoded)
            const noexcept;
        [[nodiscard]] RenderResult<void> resolveFrameBindings(RenderEffect&, const RenderGraph&) const noexcept;
        void bindCreateEffectCommand() noexcept;

        [[nodiscard]] const CapabilityGraphDescription* capabilityGraph(CapabilityImplementationId) const noexcept;
        [[nodiscard]] static RenderError
        capabilityOutputError(const CapabilityGraphDescription&, const EffectOutputDesc&) noexcept;
        [[nodiscard]] RenderResult<std::vector<RenderEffect::ResourceDependency>>
        resolveCapabilityResources(const CapabilityDescription&) const noexcept;

        void listCapabilities(const ListCapabilities::Request&, ListCapabilities::Response&) noexcept;
        void validateCapabilityConfiguration(
            const ValidateCapabilityConfiguration::Request&,
            ValidateCapabilityConfiguration::Response&
        ) noexcept;

        [[nodiscard]] RenderResult<ResourceInstanceId>
        resolveEffectResource(
            const RenderEffect&,
            std::uint32_t capability,
            std::uint32_t dependency,
            const VRenderScope&
        ) noexcept;

        [[nodiscard]] RenderResult<ResourceSet*> findScope(const VRenderScope&) noexcept;

        [[nodiscard]] RenderResult<ResourceInstanceId> ensureResource(ResourceSet&, ResourceTypeId) noexcept;

        void removeEffect(const RemoveEffect::Request&, RemoveEffect::Response&) noexcept;

        void detachEffect(const Detach::Request&, Detach::Response&) noexcept;

        void queryRenderStats(const QueryRenderStats::Request&, QueryRenderStats::Response&) noexcept;
        void queryExecutionProgress(const QueryExecutionProgress::Request&, QueryExecutionProgress::Response&) noexcept;

        void listCommands(const ListCommands::Request&, ListCommands::Response&) noexcept;
        void queryCommandIds(const QueryCommandIds::Request&, QueryCommandIds::Response&) noexcept;
        void listCommandBindings(const ListCommandBindings::Request&, ListCommandBindings::Response&) noexcept;
        void selectCommandBinding(const SelectCommandBinding::Request&, SelectCommandBinding::Response&) noexcept;

        void stopCommand(const Stop::Request&, Stop::Response&) noexcept;

        void dispatchRequest(ipc::ReceivedRequest received) noexcept;
        [[nodiscard]] RenderResult<HandlerCall>
        resolveCommandCall(CommandId, const CommandRecord&, const ipc::IpcMessage&, ResourceCommandUse& resource) noexcept;
        void failExecution(RenderError error) noexcept;

        std::size_t publishResponses(std::size_t limit) noexcept;

        void shutdown(RenderError cause = RenderError{kCommandCancelled}) noexcept;
    };
} // namespace lux::render

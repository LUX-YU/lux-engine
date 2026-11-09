#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exec/task.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/execution/ResizeResources.hpp>
#include <lux/engine/render/extensions/CapabilityRegistry.hpp>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/storage/StorageCreateContext.hpp>
#include <lux/engine/render/runtime/CommandRegistry.hpp>
#include <lux/engine/render/runtime/EffectCompilation.hpp>
#include <lux/engine/render/runtime/FrameSubmission.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/ResponseOutbox.hpp>
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
            std::span<const StorageFactory>,
            std::span<const HandlerRecord>,
            std::span<const CommandBinding>,
            std::span<const FrameDataCall> = {}
        ) const noexcept;
        void
            publishRegistration(
                std::span<const StorageFactory>,
                std::span<HandlerRecord>,
                std::span<const CommandBinding>
            ) noexcept;

        [[nodiscard]] RenderResult<void> run() noexcept;

        void stop() noexcept
        {
            stop_requested.store(true);
            ipc::IpcCore::instance().notifyWork();
        }

        [[nodiscard]] RenderServerStats stats() const noexcept;

        RenderServerConfig config;
        execution::CpuScheduler cpu_scheduler; // Borrowed from the application's ExecutionRuntime.
        ipc::IpcResponder responder;
        cxx::StableSlotMap<FeatureEntry, FeatureTag> features;
        std::map<std::string_view, FeatureId, std::less<>> features_by_name;
        CommandRegistry command_registry_;
        CapabilityRegistry capability_registry_;
        cxx::StableSlotMap<RenderScene, SceneTag> scenes;
        cxx::StableSlotMap<RenderTarget, TargetTag> targets;
        cxx::StableSlotMap<RenderEffect, EffectTag> effects;
        cxx::SlotMap<EffectAttachment, AttachmentTag> attachments;
        cxx::SlotKeySparseSet<TargetId, std::monostate> active_targets;
        std::unique_ptr<detail::DeviceSession> device;
        std::unique_ptr<detail::ResourceUploader> uploads;
        std::unique_ptr<detail::FrameExecution> frames;
        std::optional<ResourceRegistry> resources;
        std::optional<StorageSet> global_storages;
        std::optional<ipc::ReceivedRequest> deferred_request_;
        bool structural_command_running_ = false;
        bool paged_instances = false;

        [[nodiscard]] StorageCreateContext storageContext() noexcept
        {
            return {*device, uploads.get(), frames.get(), *resources, paged_instances};
        }

        ResponseOutbox response_outbox_;
        stdexec::counting_scope command_scope_;
        execution::RunLoop render_loop_;
        ipc::RequesterId frame_source_;
        CommandId submit_frame_command_;
        CommandId stop_command_;
        CommandId select_binding_command_;
        std::vector<FrameRecord> frame_inputs_;
        std::size_t first_frame_ = 0;
        std::size_t frame_count_ = 0;
        std::uint64_t last_frame_sequence_ = 0;
        bool frame_resynchronization_ = false;
        std::vector<detail::RenderGraphInstance*> frame_instances;
        std::vector<detail::SurfaceTarget*> frame_surfaces;
        std::vector<detail::StorageFrameUse> frame_storages;
        std::vector<std::pair<detail::RenderGraphInstance*, detail::FeatureBinding*>> frame_features;
        bool active_frame_inputs_dirty = true;
        std::atomic<bool> stop_requested{false}, started{false}, running{false};
        RenderError execution_error;
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};
        std::atomic<std::uint64_t> storages_created{0};
        std::atomic<std::uint64_t> presentations{0};

        Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler cpu_scheduler);

        template <RenderCommand C, auto Method> CommandId bindFrameworkCommand() noexcept
        {
            return command_registry_.registerCommand(CommandBinding::bind<C, Method>(*this));
        }

        void detachAttachment(AttachmentId id) noexcept;
        [[nodiscard]] bool canChangeAttachments(std::span<const AttachmentId>) const noexcept;

        RenderResult<runtime_detail::EffectAttachment> prepareAttachment(
            runtime_detail::RenderEffect&,
            detail::TargetOutput,
            StorageSet& scene,
            StorageSet& view,
            StorageSet& attachment,
            const AttachmentRequest& input
        ) noexcept;

        void attachEffect(const Attach::Request&, Attach::Response&) noexcept;

        using ResizeCandidates = std::vector<std::pair<AttachmentId, detail::RenderGraphInstance::ResizeResources>>;
        [[nodiscard]] RenderResult<ResizeCandidates>
            prepareTargetResize(TargetId, detail::TargetOutput, std::span<const AttachmentId>) noexcept;

        [[nodiscard]] bool
            targetDependencyCycle(TargetId, std::span<const TargetImageInput>, AttachmentId) const noexcept;

        void rebuildActiveFrameInputs() noexcept;
        [[nodiscard]] RenderResult<bool> acquireFrameOutputs(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> presentFrameOutputs() noexcept;
        RenderResult<void> submitCurrentFrame(std::uint32_t slot) noexcept;
        RenderResult<void> progressGpuCompletions() noexcept;
        void progressUploads() noexcept;
        void progressFrame() noexcept;

        auto submitFrame(SubmitFrame::Request&&) noexcept;
        void bindSubmitFrameCommand() noexcept;
        void admitFrame(RenderFrame, void*, void (*)(void*, FrameCompletion) noexcept) noexcept;
        void finishFrame(FrameRecord&, FrameCompletion) noexcept;
        [[nodiscard]] RenderResult<BoundFrameData> resolveFrameData(const FrameDataDestination&) noexcept;
        void activateFrame(FrameRecord&) noexcept;
        void dispatchFramePhases(FrameRecord&) noexcept;
        void failFrame(FrameRecord&) noexcept;
        void submitFrameRendering(FrameRecord&) noexcept;
        void prepareFramePrograms(FrameRecord&) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> prepareRasterPrograms() noexcept;

        static void NRI_CALL
        diagnostic(nri::Message severity, const char*, std::uint32_t, const char*, void* user) noexcept;

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

        auto compileGraphEffect(
            GraphEffectRequest&&,
            RenderError = {},
            std::vector<RenderEffect::Implementation> = {},
            std::vector<RenderEffect::StorageDependency> = {}
        ) noexcept;
        auto createCapabilityEffect(CreateEffect::Request&&) noexcept;
        [[nodiscard]] RenderResult<void>
        buildCapabilityGraph(
            CreateEffectRequest&,
            GraphEffectRequest&,
            std::vector<RenderEffect::StorageDependency>&
        ) noexcept;
        [[nodiscard]] RenderResult<const ConfigValue*>
        qualifyConfiguration(const VCapabilityConfiguration&, const CapabilityImplementation&, ConfigValue& decoded)
            const noexcept;
        [[nodiscard]] RenderResult<void> resolveFrameBindings(RenderEffect&, const RenderGraph&) const noexcept;
        void bindCreateEffectCommand() noexcept;

        [[nodiscard]] const CapabilityGraphDescription* capabilityGraph(CapabilityImplementationId) const noexcept;
        [[nodiscard]] static RenderError
        capabilityOutputError(const CapabilityGraphDescription&, const EffectOutputDesc&) noexcept;
        [[nodiscard]] RenderResult<std::vector<RenderEffect::StorageDependency>>
        resolveCapabilityStorage(const CapabilityImplementation&, const ConfigValue&) const noexcept;

        void listCapabilities(const ListCapabilities::Request&, ListCapabilities::Response&) noexcept;
        void
        validateCapabilityConfiguration(
            const ValidateCapabilityConfiguration::Request&,
            ValidateCapabilityConfiguration::Response&
        ) noexcept;
        [[nodiscard]] RenderResult<StorageId>
        resolveEffectStorage(
            const RenderEffect&,
            std::uint32_t capability,
            std::uint32_t dependency,
            const VRenderScope&
        ) noexcept;
        [[nodiscard]] RenderResult<StorageSet*> findScope(const VRenderScope&) noexcept;
        [[nodiscard]] RenderResult<StorageId> ensureStorage(
            StorageSet&,
            StorageTypeId,
            std::string_view,
            const ConfigValue&,
            std::vector<std::pair<StorageSet*, StorageId>>& created
        ) noexcept;

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
        resolveCommandCall(CommandId, const CommandRecord&, const ipc::IpcMessage&, void*& storage) noexcept;
        void failExecution(RenderError error) noexcept;

        std::size_t publishResponses(std::size_t limit) noexcept;

        void shutdown(RenderError cause = RenderError{kCommandCancelled}) noexcept;
    };
} // namespace lux::render

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
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/execution/ResizeResources.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/StorageCreateContext.hpp>
#include <lux/engine/render/runtime/CommandRegistry.hpp>
#include <lux/engine/render/runtime/EffectCompilation.hpp>
#include <lux/engine/render/runtime/FrameSubmission.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/ResponseOutbox.hpp>
#include <lux/engine/render/runtime/Scene.hpp>
#include <lux/engine/render/runtime/detail/CommandDispatchContext.hpp>
#include <thread>

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    struct RenderServer::Impl final
    {
        [[nodiscard]] RenderResult<void> initialize(DeviceOptions options) noexcept;
        void registerBuiltinFeatures() noexcept;
        FeatureId publishFeature(const FeatureDescriptor&, const FeatureGraphOps&) noexcept;
        RenderResult<void> validateFeature(const FeatureDescriptor&, const FeatureGraphOps&) const noexcept;
        RenderResult<void>
            validateRegistration(std::span<const StorageFactory>, std::span<const HandlerRecord>, std::span<const CommandBinding>)
                const noexcept;
        void
            publishRegistration(std::span<const StorageFactory>, std::span<HandlerRecord>, std::span<const CommandBinding>) noexcept;

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
        cxx::StableSlotMap<RenderScene, SceneTag> scenes;
        cxx::StableSlotMap<RenderTarget, TargetTag> targets;
        cxx::SlotMap<RenderEffect, EffectTag> effects;
        cxx::SlotMap<EffectAttachment, AttachmentTag> attachments;
        cxx::SlotKeySparseSet<TargetId, std::monostate> active_targets;
        std::unique_ptr<detail::DeviceSession> device;
        std::unique_ptr<detail::ResourceUploader> uploads;
        std::unique_ptr<detail::FrameExecution> frames;
        std::optional<ResourceRegistry> resources;
        std::optional<StorageSet> global_storages;
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
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};
        std::atomic<std::uint64_t> storages_created{0};
        std::atomic<std::uint64_t> presentations{0};

        Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler cpu_scheduler);

        template <RenderCommand C, auto Method> void bindFrameworkCommand() noexcept
        {
            command_registry_.registerCommand(CommandBinding::bind<C, Method>(*this));
        }

        void detachAttachment(AttachmentId id) noexcept;
        [[nodiscard]] bool canChangeAttachments(std::span<const AttachmentId>) const noexcept;

        RenderResult<StorageSet*> findStorageSet(const VStorageLocation&) noexcept;
        void findStorage(const FindStorage::Request&, FindStorage::Response&) noexcept;
        void prepareStorage(const PrepareStorage::Request&, PrepareStorage::Response&) noexcept;
        void removeStorage(const RemoveStorage::Request&, RemoveStorage::Response&) noexcept;

        RenderResult<runtime_detail::EffectAttachment> prepareAttachment(
            const runtime_detail::RenderEffect&,
            detail::TargetOutput,
            StorageSet& scene,
            StorageSet& view,
            StorageSet& attachment,
            std::span<const TargetImageInput> target_images
        ) noexcept;

        void attachEffect(const Attach::Request&, Attach::Response&) noexcept;

        using ResizeCandidates = std::vector<std::pair<AttachmentId, detail::RenderGraphInstance::ResizeResources>>;
        [[nodiscard]] RenderResult<ResizeCandidates>
            prepareTargetResize(TargetId, detail::TargetOutput, std::span<const AttachmentId>) noexcept;

        [[nodiscard]] bool targetDependencyCycle(TargetId, std::span<const TargetImageInput>, AttachmentId)
            const noexcept;

        void rebuildActiveFrameInputs() noexcept;
        RenderResult<void> submitCurrentFrame(std::uint32_t slot) noexcept;
        RenderResult<void> progressGpuCompletions() noexcept;
        void progressUploads() noexcept;
        void progressFrame() noexcept;

        auto submitFrame(SubmitFrame::Request&&) noexcept;
        void bindSubmitFrameCommand() noexcept;
        void admitFrame(FrameSubmission, void*, void (*)(void*, FrameCompletion) noexcept) noexcept;
        void finishFrame(FrameRecord&, FrameCompletion) noexcept;
        void skipFrameCommands(FrameRecord&, RenderError) noexcept;
        void activateFrame(FrameRecord&) noexcept;
        void dispatchFrameGroups(FrameRecord&) noexcept;
        void failFrame(FrameRecord&) noexcept;
        void submitFrameRendering(FrameRecord&) noexcept;

        static void NRI_CALL
        diagnostic(nri::Message severity, const char*, std::uint32_t, const char*, void* user) noexcept;

        void createScene(const CreateScene::Request&, CreateScene::Response&) noexcept;

        void removeScene(const RemoveScene::Request&, RemoveScene::Response&) noexcept;

        void createView(const CreateView::Request&, CreateView::Response&) noexcept;
        void updateAttachmentInputs(const UpdateAttachmentInputs::Request&, UpdateAttachmentInputs::Response&) noexcept;
        exec::task<AckResponse> updateDrawList(UpdateDrawList::Request&&) noexcept;
        void invalidateHistory(const InvalidateHistory::Request&, InvalidateHistory::Response&) noexcept;

        void removeView(const RemoveView::Request&, RemoveView::Response&) noexcept;

        void createTarget(const CreateTarget::Request&, CreateTarget::Response&) noexcept;

        exec::task<TargetCreated> resizeTarget(ResizeTarget::Request&&) noexcept;
        exec::task<AckResponse> removeTarget(RemoveTarget::Request&&) noexcept;
        void setTargetActive(const SetTargetActive::Request&, SetTargetActive::Response&) noexcept;

        auto createEffect(CreateEffect::Request&&) noexcept;
        void bindCreateEffectCommand() noexcept;

        void removeEffect(const RemoveEffect::Request&, RemoveEffect::Response&) noexcept;

        void detachEffect(const Detach::Request&, Detach::Response&) noexcept;

        void queryRenderStats(const QueryRenderStats::Request&, QueryRenderStats::Response&) noexcept;
        void queryExecutionProgress(const QueryExecutionProgress::Request&, QueryExecutionProgress::Response&) noexcept;

        void listFeatures(const ListFeatures::Request&, ListFeatures::Response&) noexcept;
        void listStorages(const ListStorages::Request&, ListStorages::Response&) noexcept;

        void listCommands(const ListCommands::Request&, ListCommands::Response&) noexcept;
        void listCommandBindings(const ListCommandBindings::Request&, ListCommandBindings::Response&) noexcept;
        void selectCommandBinding(const SelectCommandBinding::Request&, SelectCommandBinding::Response&) noexcept;

        void stopCommand(const Stop::Request&, Stop::Response&) noexcept;

        void dispatchRequest(ipc::ReceivedRequest received) noexcept;

        std::size_t publishResponses(std::size_t limit) noexcept;

        void shutdown(RenderError cause = RenderError{kCommandCancelled}) noexcept;
    };
} // namespace lux::render

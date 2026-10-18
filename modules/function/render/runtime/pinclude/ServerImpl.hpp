#pragma once

#include "CommandDispatchContext.hpp"
#include "CommandRegistry.hpp"
#include "Scene.hpp"
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/Error.hpp>
#include <lux/engine/render/runtime/VulkanServer.hpp>
#include <thread>

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    namespace runtime_detail
    {
        struct FeatureEntry final
        {
            // Stable table construction binds views only after their owned metadata exists.
            FeatureId id;
            std::string name;
            FeatureDescriptor descriptor;
            FeatureGraphOps graph;
            decltype(feature_detail::Implementation::programs) programs;

            FeatureEntry(feature_detail::Implementation input) noexcept
                : name(input.descriptor->type.name()), descriptor(*input.descriptor), graph(*input.graph),
                  programs(input.programs)
            {
                descriptor.type = FeatureName(name);
            }
        };

    } // namespace runtime_detail

    struct RenderServer::Impl final
    {
        [[nodiscard]] RenderResult<void> initialize(vulkan::DeviceOptions options) noexcept;
        [[nodiscard]] RenderResult<void> run() noexcept;

        void stop() noexcept
        {
            stop_requested.store(true);
            ipc::IpcCore::instance().notifyWork();
        }

        [[nodiscard]] RenderServerStats stats() const noexcept;

        RenderServerConfig config;
        execution::CpuScheduler scheduler; // Borrowed from the Host runtime.
        ipc::IpcResponder responder;
        cxx::StableSlotMap<FeatureEntry, FeatureTag> features;
        std::map<std::string_view, FeatureId, std::less<>> features_by_name;
        CommandRegistry command_registry_;
        cxx::StableSlotMap<RenderScene, SceneTag> scenes;
        cxx::StableSlotMap<RenderTarget, TargetTag> targets;
        cxx::SlotMap<RenderEffect, EffectTag> effects;
        cxx::SlotMap<EffectAttachment, AttachmentTag> attachments;
        cxx::SlotKeySparseSet<TargetId, std::monostate> active_targets;
        std::optional<vulkan::InstanceContext> instance;
        std::unique_ptr<vulkan::DeviceSession> device;
        ResponseOutbox response_outbox_;
        stdexec::counting_scope command_scope_;
        execution::RunLoop render_loop_;
        std::size_t next_target = 0;
        std::atomic<bool> stop_requested{false}, started{false}, running{false};
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};
        std::atomic<std::uint64_t> scoped_buffers_created{0};

        Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler scheduler);

        template <RenderCommand C, auto Method> void addBuiltin() noexcept
        {
            command_registry_.registerCommand(CommandBinding::bind<C, Method>(*this));
        }

        RenderResult<void> detachAttachment(AttachmentId id) noexcept;

        RenderResult<StorageSet*> findStorageSet(const VStorageOwner&) noexcept;
        void findStorage(const FindStorage::Request&, FindStorage::Response&) noexcept;
        void prepareStorage(const PrepareStorage::Request&, PrepareStorage::Response&) noexcept;
        void removeStorage(const RemoveStorage::Request&, RemoveStorage::Response&) noexcept;

        void attachEffect(const Attach::Request&, Attach::Response&) noexcept;

        RenderResult<void> resizeTargetResources(RenderTarget& target, const TargetDesc& description) noexcept;

        auto readBackOutput(Readback::Request&&) noexcept;

        RenderResult<void> advanceRendering() noexcept;

        static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT*,
            void* user
        ) noexcept;

        void createScene(const CreateScene::Request&, CreateScene::Response&) noexcept;

        void removeScene(const RemoveScene::Request&, RemoveScene::Response&) noexcept;

        void createView(const CreateView::Request&, CreateView::Response&) noexcept;
        void updateAttachmentInputs(const UpdateAttachmentInputs::Request&, UpdateAttachmentInputs::Response&) noexcept;

        void removeView(const RemoveView::Request&, RemoveView::Response&) noexcept;

        void createTarget(const CreateTarget::Request&, CreateTarget::Response&) noexcept;

        void resizeTarget(const ResizeTarget::Request&, ResizeTarget::Response&) noexcept;

        auto createEffect(CreateEffect::Request&&) noexcept;
        void installAsyncBuiltins() noexcept;
        void installReadbackCommand() noexcept;

        std::size_t drainCompletions(std::size_t budget = 128) noexcept;

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

        void shutdown() noexcept;
    };
} // namespace lux::render

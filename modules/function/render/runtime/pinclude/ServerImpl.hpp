#pragma once

#include <lux/engine/render/runtime/VulkanServer.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/runtime/Error.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include "Deferred.hpp"
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    namespace runtime_detail
    {
        struct View final
        {
            CameraState camera;
            StorageSet storages;
            View(ResourceRegistry& registry, CameraState value) noexcept : camera(value), storages(registry) {}
        };
        struct Scene final
        {
            StorageSet storages;
            cxx::StableSlotMap<View, ViewTag> views;
            explicit Scene(ResourceRegistry& registry) noexcept : storages(registry) {}
        };
        struct Target final
        {
            TargetDesc description;
            std::uint64_t epoch = 1;
        };
        struct ExecutionGroup final
        {
            SceneId scene;
            ViewId view;
            TargetId target;
            vulkan::ExecutionContext* context;
        };
        struct RenderEffect final
        {
            std::shared_ptr<const vulkan::CompiledGraph> code;
            std::vector<StorageArgument> arguments;
            std::vector<ExecutionGroup> groups;
            std::size_t next_group = 0;
        };
        struct FeatureEntry final
        {
            // Stable table construction binds views only after their owned metadata exists.
            FeatureId id;
            std::string name;
            FeatureDescriptor descriptor;
            FeatureGraphOps graph;
            decltype(feature_detail::Implementation::programs) programs;

            FeatureEntry(feature_detail::Implementation input) noexcept
                : name(input.descriptor->type.name()), descriptor(*input.descriptor),
                  graph(*input.graph), programs(input.programs)
            {
                descriptor.type = FeatureName(name);
            }
        };

        struct SupportedHandler final
        {
            HandlerId id;
            HandlerCall call;
        };

        struct CommandMetadata final
        {
            CommandId id;
            CommandHandler binding;
            std::vector<SupportedHandler> supported;
            HandlerId selected;
        };

        struct CommandEntry final
        {
            HandlerCall active;
            std::unique_ptr<CommandMetadata> cold;
        };
    } // namespace runtime_detail

    struct RenderServer::Impl final
    {
        // One handler-call borrow; every table and deferred request remains owned by this Impl.
        struct CommandCall final
        {
            Impl& server;
            RenderRequest& request;
        };

        static const CommandCompletionFns command_completion_functions;
        static const CommandCompletionFns deferred_completion_functions;

        [[nodiscard]] RenderResult<void> initialize(vulkan::DeviceOptions options) noexcept;
        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept { stop_requested.store(true); }
        [[nodiscard]] RenderServerStats stats() const noexcept;

        RenderServerConfig config;
        execution::CpuScheduler scheduler; // Borrowed from the Host runtime.
        CpuSchedule cpu;
        ipc::IpcResponder responder;
        cxx::StableSlotMap<FeatureEntry, FeatureTag> features;
        cxx::SlotMap<CommandEntry, CommandTag> command_table;
        cxx::SlotMap<std::unique_ptr<HandlerEntry>, HandlerTag> handlers;
        cxx::StableSlotMap<Scene, SceneTag> scenes;
        cxx::SlotMap<Target, TargetTag> targets;
        cxx::SlotMap<RenderEffect, EffectTag> effects;
        std::optional<vulkan::InstanceContext> instance;
        std::unique_ptr<vulkan::DeviceSession> device;
        std::vector<RenderRequest> undelivered;
        DeferredSlots deferred;
        CompletionQueue completion_events;
        std::atomic<std::size_t> completion_publishers{0};
        std::size_t next_effect = 0;
        std::atomic<bool> stop_requested{false}, started{false}, running{false};
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};
        std::atomic<std::uint64_t> scoped_buffers_created{0};

        Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler scheduler);

        template <RenderCommand C, RenderResult<void> (Impl::*Method)(RenderRequest&) noexcept>
        void addBuiltin() noexcept
        {
            auto binding = CommandHandler::bindInvocation<C>({
                [](void* owner, CommandInvocation& invocation) noexcept {
                    auto& accepted = static_cast<CommandCall*>(invocation.completion_owner)->request;
                    return (static_cast<Impl*>(owner)->*Method)(accepted);
                }, this
            });
            addCommand(binding);
        }

        CommandId addCommand(CommandHandler binding) noexcept;

        void settle(RenderRequest request, RenderResult<void> result) noexcept;
        void retryReplies() noexcept;

        RenderResult<void> withdraw(RenderEffect& effect, std::size_t index) noexcept;

        RenderResult<StorageSet*> storageOwner(const VStorageOwner&) noexcept;
        RenderResult<void> findStorage(RenderRequest&) noexcept;
        RenderResult<void> prepareStorage(RenderRequest&) noexcept;
        RenderResult<void> releasePreparedStorage(RenderRequest&) noexcept;

        RenderResult<void> attach(RenderRequest& request) noexcept;

        RenderResult<void> resize(TargetId id, Target& target, const TargetDesc& description) noexcept;

        RenderResult<void> readback(RenderRequest& request) noexcept;


        RenderResult<void> tick() noexcept;

        static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT*,
            void* user
        ) noexcept;

        RenderResult<void> createScene(RenderRequest& request) noexcept;

        RenderResult<void> removeScene(RenderRequest& request) noexcept;

        RenderResult<void> createView(RenderRequest& request) noexcept;
        RenderResult<void> updateCamera(RenderRequest& request) noexcept;

        RenderResult<void> removeView(RenderRequest& request) noexcept;

        RenderResult<void> createTarget(RenderRequest& request) noexcept;

        RenderResult<void> resizeTarget(RenderRequest& request) noexcept;

        DeferredCompletion* reserveCompletion() noexcept;

        static void completeOperation(void* value, ECommandOutcome outcome, RenderError error, void* response) noexcept;

        RenderResult<CommandInvocation*> deferCommand(RenderRequest& request) noexcept;

        struct CompileState;

        RenderResult<void> createEffect(RenderRequest& request) noexcept;

        void drainCompletions(std::size_t budget = 128) noexcept;

        RenderResult<void> removeEffect(RenderRequest& request) noexcept;

        RenderResult<void> detach(RenderRequest& request) noexcept;

        RenderResult<void> queryRenderStats(RenderRequest& request) noexcept;
        RenderResult<void> queryExecutionProgress(RenderRequest& request) noexcept;

        RenderResult<void> listFeatures(RenderRequest& request) noexcept;
        RenderResult<void> listStorages(RenderRequest& request) noexcept;
        [[nodiscard]] CommandList commandDirectory() const noexcept;

        RenderResult<void> listCommands(RenderRequest& request) noexcept;
        RenderResult<void> listCommandBindings(RenderRequest& request) noexcept;
        RenderResult<void> selectCommandBinding(RenderRequest& request) noexcept;

        RenderResult<void> stopCommand(RenderRequest& request) noexcept;

        void dispatch(ipc::ReceivedRequest received) noexcept;

        void shutdown() noexcept;
    };
} // namespace lux::render

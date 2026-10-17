#pragma once

#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include "TypedProtocol.hpp"
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
            std::array<float, 4> color;
            ResourceDomain domain;
        };
        struct Scene final
        {
            ResourceDomain domain;
            cxx::SlotMap<View, ViewTag> views;
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
            FeatureId id;
            FeatureRegistration definition;
        };

        struct SupportedHandler final
        {
            HandlerId id;
            HandlerCall call;
        };

        struct CommandMetadata final
        {
            CommandId id;
            CommandRegistration binding;
            CommandTransport transport;
            std::shared_ptr<const void> code_pin;
            const StorageDescriptor* storage = nullptr;
            bool available = true;
            std::vector<SupportedHandler> supported;
            HandlerId selected;
            const std::shared_ptr<const void>* active_code_pin = nullptr;
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

        [[nodiscard]] RenderResult<FeatureId> registerFeature(
            FeatureRegistration feature, std::span<const CommandTransport> transports
        ) noexcept;
        [[nodiscard]] RenderResult<void> registerStorage(
            StorageRegistration storage, std::span<const CommandTransport> transports
        ) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept { stop_requested.store(true); }
        [[nodiscard]] RenderServerStats stats() const noexcept;

        RenderServerConfig config;
        ipc::IpcBoundType<ValueEnvelope<RenderError>> failure_type;
        ipc::IpcResponder responder;
        SessionIdentity identity;
        cxx::StableSlotMap<FeatureEntry, FeatureTag> features;
        std::vector<StorageRegistration> storage_types;
        cxx::SlotMap<CommandEntry, CommandTag> command_table;
        cxx::SlotMap<std::unique_ptr<HandlerEntry>, HandlerTag> handlers;
        HelloResponse manifest;
        cxx::SlotMap<Scene, SceneTag> scenes;
        cxx::SlotMap<Target, TargetTag> targets;
        cxx::SlotMap<RenderEffect, EffectTag> effects;
        std::optional<vulkan::InstanceContext> instance;
        std::unique_ptr<vulkan::DeviceSession> device;
        std::vector<RenderRequest> undelivered;
        DeferredSlots deferred;
        CompletionQueue completion_events;
        std::atomic<std::size_t> completion_publishers{0};
        std::size_t compiling = 0;
        unsigned clients = 0;
        std::size_t next_effect = 0;
        std::atomic<bool> stop_requested{false}, started{false}, running{false};
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};
        std::atomic<std::uint64_t> scoped_buffers_created{0};

        Impl(RenderServerConfig value, ipc::IpcResponder input, SessionIdentity session, std::size_t pending_capacity);

        template <RenderCommand C, RenderResult<void> (Impl::*Method)(RenderRequest&) noexcept>
        void addBuiltin() noexcept
        {
            static constexpr auto handler =
                +[](Impl& owner, const typename C::Request&, Reply<C>& context) noexcept
            {
                auto& invocation = protocol_detail::ReplyAccess::invocation(context);
                return (owner.*Method)(static_cast<CommandCall*>(invocation.completion_owner)->request);
            };
            addCommand(bindCommand<C, handler>(*this), commandTransport<C>(), nullptr, config.implementation_code_pin);
        }

        CommandId addCommand(
            CommandRegistration binding,
            CommandTransport transport,
            const StorageDescriptor* storage = nullptr,
            std::shared_ptr<const void> pin = {}
        ) noexcept;

        RenderResult<void> validateHandlers(
            std::span<const CommandRegistration> handlers, std::span<const CommandTransport> transports
        ) const noexcept;

        void settle(RenderRequest request, RenderResult<void> result) noexcept;

        RenderResult<void> withdraw(RenderEffect& effect, std::size_t index) noexcept;

        template <class Predicate> RenderResult<void> admitWithdraw(Predicate matches) const noexcept
        {
            std::size_t needed = 0;
            for (const auto& effect : effects)
                for (const auto& group : effect.groups)
                    if (matches(effect, group))
                        if (auto ticket = group.context->lastUse();
                            ticket && ticket->serial() > device->queue().completed())
                            ++needed;
            const auto& retired = device->retirement();
            if (needed > retired.capacity() - retired.pending()) return cxx::unexpected(RenderError{kRuntimeCapacity});
            return {};
        }

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

        RenderResult<void> removeView(RenderRequest& request) noexcept;

        RenderResult<void> createTarget(RenderRequest& request) noexcept;

        RenderResult<void> resizeTarget(RenderRequest& request) noexcept;

        DeferredCompletion* reserveCompletion(bool for_compile = false) noexcept;

        static void completeOperation(void* value, ECommandOutcome outcome, RenderError error, void* response) noexcept;

        RenderResult<CommandInvocation*> deferCommand(RenderRequest& request, std::size_t reply_bytes) noexcept;

        struct CompileState;

        RenderResult<void> createEffect(RenderRequest& request) noexcept;

        void drainCompletions(std::size_t budget = 128) noexcept;

        RenderResult<void> removeEffect(RenderRequest& request) noexcept;

        RenderResult<void> detach(RenderRequest& request) noexcept;

        RenderResult<void> queryRenderStats(RenderRequest& request) noexcept;

        RenderResult<void> listFeatures(RenderRequest& request) noexcept;
        RenderResult<void> listCommands(RenderRequest& request) noexcept;
        RenderResult<void> listCommandBindings(RenderRequest& request) noexcept;
        RenderResult<void> selectCommandBinding(RenderRequest& request) noexcept;

        RenderResult<void> stopCommand(RenderRequest& request) noexcept;

        void dispatch(ipc::ReceivedRequest received) noexcept;

        void shutdown() noexcept;
    };
} // namespace lux::render

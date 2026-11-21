#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/cxx/execution/ManualContext.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/commands/CommandBindingLocation.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/CommandHandler.hpp>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/extensions/resource/ResourceIdentity.hpp>
#include <stdexec/execution.hpp>
#include <lux/engine/render/extensions/resource/ReplicaMutation.hpp>

namespace lux::render::runtime_detail
{
    class CommandRegistry;
} // namespace lux::render::runtime_detail

namespace lux::render
{
    struct HandlerCall final
    {
        void (*invoke)(void*, runtime_detail::CommandDispatchContext&) noexcept = nullptr;
        void* object = nullptr;
    };

    template <class C>
    concept EffectCommand = RenderCommand<C> && requires(const typename C::Request& request)
    {
        { C::effectId(request) } noexcept -> std::same_as<EffectId>;
        { C::nodeId(request) } noexcept -> std::same_as<graph::NodeId>;
    } && !requires(const typename C::Request& request) { C::resourceId(request); };

    template <class C, class State, auto Method>
    concept EffectCommandMethodFor = EffectCommand<C> &&
        std::same_as<decltype(Method), void (*)(State&, const typename C::Request&, typename C::Response&) noexcept>;

    template <class C, class State, auto Method> requires EffectCommandMethodFor<C, State, Method>
    struct EffectCommandMethod final
    {
        using Command = C;
        using Handler = void;
        using EffectState = State;
        static constexpr auto method = Method;
    };

    // Non-owning typed dispatch. The registry retains the Handler; accepted asynchronous operations
    // retain their own Request/Response state in the Server scope. No caller-thread reply publication.
    class CommandBinding final
    {
    public:
        [[nodiscard]] const CommandDescriptor* descriptor() const noexcept
        {
            return descriptor_;
        }

        [[nodiscard]] HandlerCall call() const noexcept
        {
            return call_;
        }

        [[nodiscard]] cxx::basic_type_info handlerType() const noexcept
        {
            return handler_type_;
        }

        // Identifies the typed C/member association across registration calls in this build.
        [[nodiscard]] cxx::basic_type_info methodType() const noexcept
        {
            return method_type_;
        }

        [[nodiscard]] cxx::basic_type_info storageType() const noexcept
        {
            return storage_type_;
        }

        [[nodiscard]] bool isResourceCommand() const noexcept
        {
            return storage_ != nullptr;
        }

        [[nodiscard]] bool isNodeEffectCommand() const noexcept { return effect_node_ != nullptr; }

        [[nodiscard]] bool valid() const noexcept
        {
            return descriptor_ && call_.invoke;
        }

        template <class C, class State, auto Method> requires EffectCommandMethodFor<C, State, Method>
        [[nodiscard]] static CommandBinding declareForEffect() noexcept
        {
            CommandBinding result;
            result.initializeDeclaration<C>();
            result.method_type_ = cxx::make_basic_type_info<EffectCommandMethod<C, State, Method>>();
            result.effect_state_type_ = cxx::make_basic_type_info<State>();
            result.effect_ = [](const platform::ipc::IpcMessage& message) noexcept
            {
                return C::effectId(message.get<typename C::Request>());
            };
            result.effect_node_ = [](const platform::ipc::IpcMessage& message) noexcept
            {
                return C::nodeId(message.get<typename C::Request>());
            };
            result.call_.invoke = [](void* object, runtime_detail::CommandDispatchContext& context) noexcept
            {
                auto& output = runtime_detail::prepareResponse(context.outbox, context.response_route);
                auto& response = output.emplace<typename C::Response>();
                response.error = RenderError{kRuntimeState};
                Method(*static_cast<State*>(object), context.request.get<typename C::Request>(), response);
            };
            return result;
        }

        void makeError(platform::ipc::IpcMessage& output, RenderError error) const noexcept
        {
            make_error_(output, error);
        }

        template <
            RenderCommand C,
            auto Method,
            class H = typename extension_detail::CommandInvocation<C, Method>::Handler>
            requires(SyncCommandHandlerFor<C, H, Method> || AsyncCommandHandlerFor<C, H, Method>)
        [[nodiscard]] static CommandBinding declare() noexcept
        {
            CommandBinding result;
            result.initializeDeclaration<C>();
            if constexpr (!std::is_void_v<H>)
            {
                result.handler_type_ = cxx::make_basic_type_info<H>();
            }
            result.method_type_ = cxx::make_basic_type_info<CommandMethod<C, Method>>();
            using Invocation = extension_detail::CommandInvocation<C, Method>;
            using Storage = typename Invocation::Storage;
            if constexpr (!std::is_void_v<Storage>)
            {
                result.storage_type_ = cxx::make_basic_type_info<std::remove_const_t<Storage>>();
            }
            if constexpr (requires(const typename C::Request& request) { C::resourceId(request); })
            {
                result.storage_ = [](const platform::ipc::IpcMessage& message) noexcept
                {
                    const auto resource = C::resourceId(message.get<typename C::Request>());
                    return extension_detail::ResourceIdentity::parent(resource);
                };
            }
            result.call_ = {
                [](void* object, runtime_detail::CommandDispatchContext& context) noexcept
                {
                    auto& input = context.request.get<typename C::Request>();
                    if constexpr (SyncCommandHandlerFor<C, H, Method>)
                    {
                        typename C::Response response{};
                        response.error = RenderError{kRuntimeState};
                        if constexpr (std::is_void_v<H>)
                        {
                            Invocation::invoke(context.resource, input, response);
                        }
                        else
                        {
                            Invocation::invoke(*static_cast<H*>(object), context.resource, input, response);
                        }
                        if constexpr (!std::is_void_v<Storage>)
                            runtime_detail::finishResourceCommand(context.resources, context.resource_id);
                        runtime_detail::prepareResponse(context.outbox, context.response_route)
                            .emplace<typename C::Response>(std::move(response));
                    }
                    else
                    {
                        // Admission is serialized with scope close on the RenderThread.
                        const auto route = context.response_route;
                        auto& outbox = context.outbox;
                        const auto render_scheduler =
                            context.server_delivery;
                        // Keep the request in operation state: a lazy Handler may borrow its fields
                        // after dispatchRequest has released the IPC message.
                        auto sender = stdexec::just(std::move(input)) | stdexec::let_value(
                            [object, storage = context.resource, device = context.device, render_scheduler]
                            (typename C::Request& request) noexcept
                            {
                                if constexpr (requires { Invocation::writes_replica; })
                                {
                                    return invokeReplica<C, Method>(
                                        *device, *static_cast<Storage*>(storage), request, render_scheduler
                                    );
                                }
                                else if constexpr (std::is_void_v<H>)
                                {
                                    return Invocation::invoke(storage, std::move(request));
                                }
                                else
                                {
                                    return Invocation::invoke(*static_cast<H*>(object), storage, std::move(request));
                                }
                            }
                        );
                        auto normalized =
                            std::move(sender) | stdexec::upon_error(ResponseError<C>{}) |
                            stdexec::upon_stopped(
                                []() noexcept -> typename C::Response
                                { return ResponseError<C>{}(RenderError{kCommandCancelled}); }
                            ) |
                            stdexec::continues_on(render_scheduler) | stdexec::upon_error(ResponseError<C>{}) |
                            stdexec::then(
                                [route, &outbox, &resources = context.resources, id = context.resource_id]
                                (typename C::Response response) noexcept
                                {
                                    if constexpr (!std::is_void_v<Storage>)
                                        runtime_detail::finishResourceCommand(resources, id);
                                    runtime_detail::prepareResponse(outbox, route)
                                        .emplace<typename C::Response>(std::move(response));
                                }
                            );
                        stdexec::spawn(
                            std::move(normalized),
                            context.scope.get_token(),
                            stdexec::env{stdexec::prop{stdexec::get_scheduler, render_scheduler}}
                        );
                    }
                },
                nullptr
            };
            return result;
        }

        // Framework commands borrow an already existing Impl. Professional Handler declarations
        // use declare(); the registry supplies their object only when activating the binding.
        template <RenderCommand C, auto Method, class H>
            requires(SyncCommandHandlerFor<C, H, Method> || AsyncCommandHandlerFor<C, H, Method>)
        [[nodiscard]] static CommandBinding bind(H& handler) noexcept
        {
            auto result = declare<C, Method, H>();
            result.call_.object = &handler;
            return result;
        }

    private:
        template <class C, auto Method, class S>
        static exec::task<typename C::Response>
        invokeReplica(
            detail::DeviceEntry& device,
            S& storage,
            typename C::Request& request,
            cxx::execution::ManualContext::Scheduler delivery
        ) noexcept
        {
            ReplicaMutation mutation(device);
            auto completed = Method(storage, mutation, std::move(request)) | stdexec::upon_error(ResponseError<C>{}) |
                stdexec::upon_stopped([]() noexcept { return ResponseError<C>{}(RenderError{kCommandCancelled}); });
            co_return co_await (std::move(completed) | stdexec::continues_on(delivery));
        }

        friend class ResourceRegistry;
        friend class runtime_detail::CommandRegistry;

        template <RenderCommand C> void initializeDeclaration() noexcept
        {
            descriptor_ = &commandDescriptor<C>();
            make_error_ = [](platform::ipc::IpcMessage& output, RenderError error) noexcept
            {
                output.emplace<typename C::Response>().error = error;
            };
        }

        template <RenderCommand C> struct ResponseError final
        {
            typename C::Response operator()(RenderError error) const noexcept
            {
                typename C::Response response{};
                response.error = error;
                return response;
            }

            [[noreturn]] typename C::Response operator()(cxx::execution::ScheduleError) const noexcept
            {
                // A necessary delivery was rejected: shutdown closed the Context too early.
                std::terminate();
            }

            [[noreturn]] typename C::Response operator()(std::exception_ptr) const noexcept
            {
                std::terminate();
            }
        };

        const CommandDescriptor* descriptor_ = nullptr;
        cxx::basic_type_info handler_type_;
        cxx::basic_type_info storage_type_;
        cxx::basic_type_info method_type_;
        cxx::basic_type_info effect_state_type_;
        HandlerCall call_;
        void (*make_error_)(platform::ipc::IpcMessage&, RenderError) noexcept = nullptr;
        EffectId (*effect_)(const platform::ipc::IpcMessage&) noexcept = nullptr;
        ResourceInstanceId (*storage_)(const platform::ipc::IpcMessage&) noexcept = nullptr;
        graph::NodeId (*effect_node_)(const platform::ipc::IpcMessage&) noexcept = nullptr;
    };
} // namespace lux::render

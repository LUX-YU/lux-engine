#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/CommandHandler.hpp>
#include <lux/engine/render/extensions/storage/ResourceIdentity.hpp>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <stdexec/execution.hpp>

namespace lux::render::runtime_detail
{
    class CommandRegistry;
} // namespace lux::render::runtime_detail

namespace lux::render
{
    // An address in the existing Command table, not a new identity or Handler owner.
    // Binding indices never move after publication; the CommandId qualifies the table generation.
    struct CommandBindingLocation final
    {
        CommandId command;
        std::uint32_t binding = 0;
        bool operator==(const CommandBindingLocation&) const noexcept = default;
    };

    struct HandlerCall final
    {
        void (*invoke)(void*, runtime_detail::CommandDispatchContext&) noexcept = nullptr;
        void* object = nullptr;
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

        [[nodiscard]] cxx::type_info handlerType() const noexcept
        {
            return handler_type_;
        }

        // Identifies the typed C/member association across registration calls in this build.
        [[nodiscard]] cxx::type_info methodType() const noexcept
        {
            return method_type_;
        }

        [[nodiscard]] cxx::type_info storageType() const noexcept
        {
            return storage_type_;
        }

        [[nodiscard]] std::uint32_t storageDependency() const noexcept
        {
            return storage_dependency_;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return descriptor_ && call_.invoke;
        }

        void makeError(platform::ipc::IpcMessage& output, RenderError error) const noexcept
        {
            make_error_(output, error);
        }

        template <RenderCommand C, auto Method, class H, std::uint32_t StorageDependency = UINT32_MAX>
            requires(SyncCommandHandlerFor<C, H, Method> || AsyncCommandHandlerFor<C, H, Method>)
        [[nodiscard]] static CommandBinding declare() noexcept
        {
            CommandBinding result;
            result.descriptor_ = &commandDescriptor<C>();
            result.handler_type_ = cxx::make_basic_type_info<H>();
            result.method_type_ = cxx::make_basic_type_info<CommandMethod<C, Method>>();
            result.storage_dependency_ = StorageDependency;
            using Invocation = extension_detail::CommandInvocation<C, Method>;
            using Storage = typename Invocation::Storage;
            if constexpr (!std::is_void_v<Storage>)
            {
                static_assert(CapabilityCommand<C> && StorageDependency != UINT32_MAX);
                result.storage_type_ = cxx::make_basic_type_info<std::remove_const_t<Storage>>();
            }
            result.make_error_ = [](platform::ipc::IpcMessage& output, RenderError error) noexcept
            { output.emplace<typename C::Response>().error = error; };
            if constexpr (requires(const typename C::Request& request) { C::implementationId(request); })
            {
                result.implementation_ = [](const platform::ipc::IpcMessage& message) noexcept
                { return C::implementationId(message.get<typename C::Request>()); };
            }
            if constexpr (requires(const typename C::Request& request) { C::sceneId(request); })
            {
                result.scene_ = [](const platform::ipc::IpcMessage& message) noexcept
                { return C::sceneId(message.get<typename C::Request>()); };
            }
            if constexpr (requires(const typename C::Request& request) { C::resourceId(request); })
            {
                result.storage_ = [](const platform::ipc::IpcMessage& message) noexcept
                {
                    const auto resource = C::resourceId(message.get<typename C::Request>());
                    return extension_detail::ResourceIdentity::storage(resource);
                };
            }
            if constexpr (!std::is_void_v<Storage>)
            {
                static_assert(requires(const typename C::Request& request) { C::implementationId(request); } ||
                    requires(const typename C::Request& request) { C::resourceId(request); });
            }
            result.call_ = {
                [](void* object, runtime_detail::CommandDispatchContext& context) noexcept
                {
                    auto& input = context.request.get<typename C::Request>();
                    if constexpr (SyncCommandHandlerFor<C, H, Method>)
                    {
                        auto& output = runtime_detail::prepareResponse(context.outbox, context.response_route);
                        auto& response = output.emplace<typename C::Response>();
                        response.error = RenderError{kRuntimeState};
                        Invocation::invoke(*static_cast<H*>(object), context.storage, input, response);
                        if (context.structural_command_running)
                        {
                            *context.structural_command_running = false;
                        }
                    }
                    else
                    {
                        // Admission is serialized with scope close on the RenderThread.
                        const auto route = context.response_route;
                        auto& outbox = context.outbox;
                        const auto render_scheduler = context.render_loop.scheduler();
                        auto sender = Invocation::invoke(*static_cast<H*>(object), context.storage, std::move(input));
                        auto normalized = std::move(sender) | stdexec::upon_error(ResponseError<C>{}) |
                                          stdexec::upon_stopped(
                                              []() noexcept -> typename C::Response
                                              { return ResponseError<C>{}(RenderError{kCommandCancelled}); }
                                          ) |
                                          stdexec::continues_on(render_scheduler) |
                                          stdexec::then(
                                              [route, &outbox, structural = context.structural_command_running](
                                                  typename C::Response response
                                              ) noexcept
                                              {
                                                  runtime_detail::prepareResponse(outbox, route)
                                                      .emplace<typename C::Response>(std::move(response));
                                                  if (structural)
                                                  {
                                                      *structural = false;
                                                  }
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
        friend class runtime_detail::CommandRegistry;

        template <RenderCommand C> struct ResponseError final
        {
            typename C::Response operator()(RenderError error) const noexcept
            {
                typename C::Response response{};
                response.error = error;
                return response;
            }

            [[noreturn]] typename C::Response operator()(std::exception_ptr) const noexcept
            {
                std::terminate();
            }
        };

        const CommandDescriptor* descriptor_ = nullptr;
        cxx::type_info handler_type_;
        cxx::type_info storage_type_;
        cxx::type_info method_type_;
        std::uint32_t storage_dependency_ = UINT32_MAX;
        HandlerCall call_;
        void (*make_error_)(platform::ipc::IpcMessage&, RenderError) noexcept = nullptr;
        CapabilityImplementationId (*implementation_)(const platform::ipc::IpcMessage&) noexcept = nullptr;
        SceneId (*scene_)(const platform::ipc::IpcMessage&) noexcept = nullptr;
        StorageId (*storage_)(const platform::ipc::IpcMessage&) noexcept = nullptr;
    };
} // namespace lux::render

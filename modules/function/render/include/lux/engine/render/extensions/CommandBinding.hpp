#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/CommandHandler.hpp>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <stdexec/execution.hpp>

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

        [[nodiscard]] cxx::TypeToken handlerType() const noexcept
        {
            return handler_type_;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return descriptor_ && call_.invoke;
        }

        void makeError(platform::ipc::IpcMessage& output, RenderError error) const noexcept
        {
            make_error_(output, error);
        }

        template <RenderCommand C, auto Method, class H>
            requires(SyncCommandHandlerFor<C, H, Method> || AsyncCommandHandlerFor<C, H, Method>)
        [[nodiscard]] static CommandBinding declare() noexcept
        {
            CommandBinding result;
            result.descriptor_ = &commandDescriptor<C>();
            result.handler_type_ = cxx::typeToken<H>();
            result.make_error_ = [](platform::ipc::IpcMessage& output, RenderError error) noexcept
            { output.emplace<typename C::Response>().error = error; };
            result.call_ = {
                [](void* object, runtime_detail::CommandDispatchContext& context) noexcept
                {
                    auto& input = context.request.get<typename C::Request>();
                    if constexpr (SyncCommandHandlerFor<C, H, Method>)
                    {
                        auto& output = runtime_detail::prepareResponse(context.outbox, context.response_route);
                        auto& response = output.emplace<typename C::Response>();
                        response.error = RenderError{kRuntimeState};
                        (static_cast<H*>(object)->*Method)(input, response);
                        runtime_detail::completeFrameCommand(context.frame, response.error);
                    }
                    else
                    {
                        // Admission is serialized with scope close on the RenderThread.
                        const auto route = context.response_route;
                        auto& outbox = context.outbox;
                        const auto render_scheduler = context.render_loop.scheduler();
                        auto sender = (static_cast<H*>(object)->*Method)(std::move(input));
                        auto normalized =
                            std::move(sender) | stdexec::upon_error(ResponseError<C>{}) |
                            stdexec::upon_stopped(
                                []() noexcept -> typename C::Response
                                { return ResponseError<C>{}(RenderError{kCommandCancelled}); }
                            ) |
                            stdexec::continues_on(render_scheduler) |
                            stdexec::then(
                                [route, &outbox, frame = context.frame](typename C::Response response) noexcept
                                {
                                    const auto error = response.error;
                                    runtime_detail::prepareResponse(outbox, route)
                                        .emplace<typename C::Response>(std::move(response));
                                    runtime_detail::completeFrameCommand(frame, error);
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

            typename C::Response operator()(CommandFailure failure) const noexcept
            {
                return (*this)(failure.outcome_unknown ? RenderError{kOutcomeUnknown} : failure.error);
            }

            [[noreturn]] typename C::Response operator()(std::exception_ptr) const noexcept
            {
                std::terminate();
            }
        };

        const CommandDescriptor* descriptor_ = nullptr;
        cxx::TypeToken handler_type_;
        HandlerCall call_;
        void (*make_error_)(platform::ipc::IpcMessage&, RenderError) noexcept = nullptr;
    };
} // namespace lux::render

#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/commands/CommandMethod.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <stdexec/execution.hpp>

namespace lux::render::runtime_detail
{
    class CommandRegistry;
    struct CommandDispatchContext;
    struct ResponseDelivery;
    LUX_RENDER_PUBLIC platform::ipc::IpcMessage& requestMessage(CommandDispatchContext&) noexcept;
    LUX_RENDER_PUBLIC platform::ipc::IpcMessage& prepareResponse(CommandDispatchContext&) noexcept;
    LUX_RENDER_PUBLIC ResponseDelivery& prepareCompletion(CommandDispatchContext&) noexcept;
    LUX_RENDER_PUBLIC platform::ipc::IpcMessage& completionResult(ResponseDelivery&) noexcept;
    LUX_RENDER_PUBLIC stdexec::counting_scope& commandScope(CommandDispatchContext&) noexcept;
    LUX_RENDER_PUBLIC execution::RunLoop::Scheduler renderScheduler(CommandDispatchContext&) noexcept;
    LUX_RENDER_PUBLIC void publishCompletion(ResponseDelivery&) noexcept;
} // namespace lux::render::runtime_detail

namespace lux::render
{
    template <class C, class H, auto Method>
    concept AsyncCommandHandlerFor =
        RenderCommand<C> && std::is_nothrow_invocable_v<decltype(Method), H&, typename C::Request&&> &&
        std::same_as<
            decltype(Method),
            std::invoke_result_t<decltype(Method), H&, typename C::Request&&> (H::*)(typename C::Request&&) noexcept> &&
        stdexec::sender<std::invoke_result_t<decltype(Method), H&, typename C::Request&&>> &&
        std::same_as<
            stdexec::value_types_of_t<
                std::invoke_result_t<decltype(Method), H&, typename C::Request&&>,
                stdexec::env<stdexec::prop<stdexec::get_scheduler_t, execution::RunLoop::Scheduler>>,
                std::tuple,
                std::variant>,
            std::variant<std::tuple<typename C::Response>>>;

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
                    auto& input = runtime_detail::requestMessage(context).get<typename C::Request>();
                    if constexpr (SyncCommandHandlerFor<C, H, Method>)
                    {
                        auto& output = runtime_detail::prepareResponse(context);
                        auto& response = output.emplace<typename C::Response>();
                        response.error = RenderError{kRuntimeState};
                        (static_cast<H*>(object)->*Method)(input, response);
                    }
                    else
                    {
                        // Admission is serialized with scope close on the RenderThread.
                        auto& completion = runtime_detail::prepareCompletion(context);
                        auto sender = (static_cast<H*>(object)->*Method)(std::move(input));
                        auto normalized =
                            std::move(sender) | stdexec::upon_error(ResponseError<C>{}) |
                            stdexec::upon_stopped(
                                []() noexcept -> typename C::Response
                                { return ResponseError<C>{}(RenderError{kCommandCancelled}); }
                            ) |
                            stdexec::then(
                                [record = &completion](typename C::Response response) noexcept
                                {
                                    runtime_detail::completionResult(*record).emplace<typename C::Response>(
                                        std::move(response)
                                    );
                                    runtime_detail::publishCompletion(*record);
                                }
                            );
                        const auto render_scheduler = runtime_detail::renderScheduler(context);
                        stdexec::spawn(
                            std::move(normalized),
                            runtime_detail::commandScope(context).get_token(),
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

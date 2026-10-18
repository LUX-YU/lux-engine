#pragma once

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/commands/CommandMethod.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <stdexec/execution.hpp>

namespace lux::render::runtime_detail
{
    class CommandRegistry;
    struct CommandDispatchContext;
    struct CommandCompletion;
    platform::ipc::IpcMessage& requestMessage(CommandDispatchContext&) noexcept;
    platform::ipc::IpcMessage& prepareResponse(CommandDispatchContext&) noexcept;
    CommandCompletion& prepareCompletion(CommandDispatchContext&) noexcept;
    platform::ipc::IpcMessage& completionResult(CommandCompletion&) noexcept;
    stdexec::counting_scope& commandScope(CommandDispatchContext&) noexcept;
    execution::RunLoop::Scheduler renderScheduler(CommandDispatchContext&) noexcept;
    void publishCompletion(CommandCompletion&) noexcept;
} // namespace lux::render::runtime_detail

namespace lux::render
{
    template <class C, class H, auto Method>
    concept AsyncCommandHandlerFor =
        RenderCommand<C> && std::constructible_from<typename C::Response, cxx::unexpected<RenderError>> &&
        std::is_nothrow_invocable_v<decltype(Method), H&, typename C::Request&&> &&
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
        [[nodiscard]] static CommandBinding bind(H& handler) noexcept
        {
            CommandBinding result;
            result.descriptor_ = &commandDescriptor<C>();
            result.make_error_ = [](platform::ipc::IpcMessage& output, RenderError error) noexcept
            { output.emplace<typename C::Response>(cxx::unexpected(error)); };
            result.call_ = {
                [](void* object, runtime_detail::CommandDispatchContext& context) noexcept
                {
                    auto& input = runtime_detail::requestMessage(context).get<typename C::Request>();
                    if constexpr (SyncCommandHandlerFor<C, H, Method>)
                    {
                        auto& output = runtime_detail::prepareResponse(context);
                        auto& response =
                            output.emplace<typename C::Response>(cxx::unexpected(RenderError{kRuntimeState}));
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
                                { return cxx::unexpected(RenderError{kCommandCancelled}); }
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
                        const auto scheduler = runtime_detail::renderScheduler(context);
                        stdexec::spawn(
                            std::move(normalized),
                            runtime_detail::commandScope(context).get_token(),
                            stdexec::env{stdexec::prop{stdexec::get_scheduler, scheduler}}
                        );
                    }
                },
                &handler
            };
            return result;
        }

    private:
        friend class runtime_detail::CommandRegistry;

        template <RenderCommand C> struct ResponseError final
        {
            typename C::Response operator()(RenderError error) const noexcept
            {
                return cxx::unexpected(error);
            }

            typename C::Response operator()(CommandFailure failure) const noexcept
            {
                return cxx::unexpected(failure.outcome_unknown ? RenderError{kOutcomeUnknown} : failure.error);
            }

            [[noreturn]] typename C::Response operator()(std::exception_ptr) const noexcept
            {
                std::terminate();
            }
        };

        const CommandDescriptor* descriptor_ = nullptr;
        HandlerCall call_;
        void (*make_error_)(platform::ipc::IpcMessage&, RenderError) noexcept = nullptr;
    };
} // namespace lux::render

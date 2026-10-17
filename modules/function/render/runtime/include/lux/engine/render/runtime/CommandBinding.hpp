#pragma once

#include <lux/engine/core/execution/WorkSchedule.hpp>
#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/protocol/CommandMethod.hpp>
#include <lux/engine/render/runtime/Error.hpp>
#include <stdexec/execution.hpp>

namespace lux::render::runtime_detail
{
    struct CommandDispatchContext;
    struct PendingOperation;
    platform::ipc::IpcMessage& requestMessage(CommandDispatchContext&) noexcept;
    platform::ipc::IpcMessage& prepareResponse(CommandDispatchContext&) noexcept;
    PendingOperation& createPendingOperation(CommandDispatchContext&) noexcept;
    platform::ipc::IpcMessage& operationResult(PendingOperation&) noexcept;
    stdexec::inplace_stop_token operationStopToken(PendingOperation&) noexcept;
    execution::CompletionPost<RenderError> operationPost(PendingOperation&) noexcept;
    void ownOperation(PendingOperation&, void*, void (*)(void*) noexcept) noexcept;
    void publishOperation(PendingOperation&) noexcept;
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
                stdexec::env<>,
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
                        // Acquire the stable completion landing place before invoking business work.
                        auto& slot = runtime_detail::createPendingOperation(context);
                        auto sender = (static_cast<H*>(object)->*Method)(std::move(input));
                        using Operation = stdexec::connect_result_t<decltype(sender), Receiver<C>>;
                        auto* operation = new Operation(stdexec::connect(std::move(sender), Receiver<C>{&slot}));
                        runtime_detail::ownOperation(
                            slot,
                            operation,
                            [](void* value) noexcept { delete static_cast<Operation*>(value); }
                        );
                        stdexec::start(*operation);
                        // A synchronous terminal only queues an event; it never drains on this call stack.
                    }
                },
                &handler
            };
            return result;
        }

    private:
        template <RenderCommand C> struct Receiver final
        {
            using receiver_concept = stdexec::receiver_t;
            runtime_detail::PendingOperation* slot;

            struct Env final
            {
                runtime_detail::PendingOperation* slot;

                stdexec::inplace_stop_token query(stdexec::get_stop_token_t) const noexcept
                {
                    return runtime_detail::operationStopToken(*slot);
                }

                execution::CompletionPost<RenderError> query(execution::GetCompletionPost) const noexcept
                {
                    return runtime_detail::operationPost(*slot);
                }
            };

            Env get_env() const noexcept
            {
                return {slot};
            }

            void set_value(typename C::Response response) && noexcept
            {
                runtime_detail::operationResult(*slot).emplace<typename C::Response>(std::move(response));
                runtime_detail::publishOperation(*slot);
            }

            void set_error(RenderError error) && noexcept
            {
                runtime_detail::operationResult(*slot).emplace<typename C::Response>(cxx::unexpected(error));
                runtime_detail::publishOperation(*slot);
            }

            void set_error(CommandFailure failure) && noexcept
            {
                std::move(*this).set_error(failure.outcome_unknown ? RenderError{kOutcomeUnknown} : failure.error);
            }

            void set_error(std::exception_ptr) && noexcept
            {
                std::terminate();
            }

            void set_stopped() && noexcept
            {
                std::move(*this).set_error(RenderError{kCommandCancelled});
            }
        };

        const CommandDescriptor* descriptor_ = nullptr;
        HandlerCall call_;
        void (*make_error_)(platform::ipc::IpcMessage&, RenderError) noexcept = nullptr;
    };
} // namespace lux::render

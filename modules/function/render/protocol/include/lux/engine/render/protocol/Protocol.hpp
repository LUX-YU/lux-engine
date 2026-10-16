#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    struct CommandTag;
    struct CommandNameTag;
    struct RequestTag;
    using CommandId = cxx::SlotKey<CommandTag>;
    using CommandName = cxx::SchemaIdView<CommandNameTag>;
    using RequestId = cxx::SlotKey<RequestTag>;
    using SessionIdentity = std::array<std::uint64_t, 2>;

    struct Ack final {};

    enum class ECompletionContract : std::uint8_t { STATE_COMMITTED, RESOURCE_GPU_READY, RESULT_AVAILABLE };
    enum class ECommandOutcome : std::uint8_t { SUCCESS, FAILED, CANCELLED, OUTCOME_UNKNOWN };

    struct CommandValueType final
    {
        cxx::TypeToken type;
        std::size_t size = 0;
        std::size_t alignment = 0;
        bool operator==(const CommandValueType&) const noexcept = default;
    };

    // Static declaration metadata. Runtime owns its copied manifest; IPC separately
    // validates the concrete module ABI and canonical move/destroy table.
    struct CommandDescriptor final
    {
        CommandName name;
        cxx::SchemaVersion version;
        CommandValueType request;
        CommandValueType response;
        ECompletionContract completion = ECompletionContract::STATE_COMMITTED;
    };

    template <typename C>
    concept RenderCommand = requires {
        typename C::Request;
        typename C::Response;
        std::string_view{C::name};
    } && std::is_nothrow_move_constructible_v<typename C::Request> &&
        std::is_nothrow_destructible_v<typename C::Request> &&
        std::is_nothrow_move_constructible_v<typename C::Response> &&
        std::is_nothrow_destructible_v<typename C::Response>;

    template <RenderCommand C>
    inline constexpr CommandDescriptor kCommandDescriptor = [] {
        cxx::SchemaVersion version{1, 0};
        ECompletionContract completion = ECompletionContract::STATE_COMMITTED;
        if constexpr (requires { C::version; }) version = C::version;
        if constexpr (requires { C::completion; }) completion = C::completion;
        return CommandDescriptor{
            CommandName(C::name), version,
            {cxx::typeToken<typename C::Request>(), sizeof(typename C::Request), alignof(typename C::Request)},
            {cxx::typeToken<typename C::Response>(), sizeof(typename C::Response), alignof(typename C::Response)},
            completion
        };
    }();

    template <RenderCommand C>
    [[nodiscard]] constexpr const CommandDescriptor& commandDescriptor() noexcept
    {
        return kCommandDescriptor<C>;
    }

    // Nontrivial values explicitly account for vector/string capacities and full
    // retained backing, including backing beyond a visible subrange.
    template <typename T>
    struct CommandValueTraits
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const T&) noexcept
            requires std::is_trivially_copyable_v<T>
        {
            return sizeof(T);
        }
    };

    struct RequestHeader final
    {
        RequestId request;
        CommandId command;
    };

    struct ReplyHeader final
    {
        RequestId request;
        ECommandOutcome outcome = ECommandOutcome::OUTCOME_UNKNOWN;
        RenderError error{};
    };

    struct CommandAsyncOps;
    namespace protocol_detail { struct CommandContextAccess; }

    struct CommandCompletionFns final
    {
        RenderResult<void> (*finish)(void*, ECommandOutcome, RenderError, void*) noexcept;
        bool (*active)(const void*) noexcept;
        RenderResult<void> (*defer)(void*, void*, const CommandAsyncOps&) noexcept = nullptr;
    };

    // A handler-call borrow, never retained by asynchronous work. The extension
    // is the existing Native completion bridge, not a service directory.
    struct CommandInvocation final
    {
        const void* request_value;
        void* completion_owner;
        const CommandCompletionFns* completion;
        void* completion_extension = nullptr;
    };

    template <RenderCommand C>
    class CommandContext final
    {
    public:
        explicit CommandContext(CommandInvocation& invocation) noexcept : invocation_(&invocation) {}
        CommandContext(const CommandContext&) = delete;
        CommandContext& operator=(const CommandContext&) = delete;

        [[nodiscard]] const typename C::Request& request() const noexcept
        {
            return *static_cast<const typename C::Request*>(invocation_->request_value);
        }

        [[nodiscard]] bool active() const noexcept
        {
            return invocation_->completion->active(invocation_->completion_owner);
        }

        // Capacity rejection preserves the response. After transfer, the original
        // reply owner retains it even if the peer closes before publication.
        // GPU_READY cannot succeed through this ordinary completion.
        [[nodiscard]] RenderResult<void> complete(typename C::Response& response) noexcept
        {
            return invocation_->completion->finish(
                invocation_->completion_owner, ECommandOutcome::SUCCESS, {}, &response
            );
        }

        [[nodiscard]] RenderResult<void> complete(typename C::Response&& response) noexcept
        {
            return complete(response);
        }

        [[nodiscard]] RenderResult<void> fail(RenderError error) noexcept
        {
            return invocation_->completion->finish(
                invocation_->completion_owner, ECommandOutcome::FAILED, error, nullptr
            );
        }

        [[nodiscard]] RenderResult<void> cancel() noexcept
        {
            return invocation_->completion->finish(
                invocation_->completion_owner, ECommandOutcome::CANCELLED, {}, nullptr
            );
        }

        // Defined by AsyncCommand.hpp for callers that actually create finite stdexec work.
        template <typename Sender>
        [[nodiscard]] RenderResult<void> completeWhen(Sender&& sender) noexcept;

    private:
        friend struct protocol_detail::CommandContextAccess;
        CommandInvocation* invocation_;
    };

    namespace protocol_detail { struct CommandHandlerAudit; }

    class CommandHandler
    {
    public:
        CommandHandler() noexcept = default;
        [[nodiscard]] const CommandDescriptor* descriptor() const noexcept { return descriptor_; }
        [[nodiscard]] bool valid() const noexcept { return descriptor_ && invoke_; }
        [[nodiscard]] RenderResult<void> invoke(CommandInvocation& invocation) const noexcept
        {
            return invoke_(user_, invocation);
        }

        template <RenderCommand C, auto Function, class State>
            requires std::same_as<decltype(Function),
                RenderResult<void> (*)(State&, const typename C::Request&, CommandContext<C>&) noexcept>
        [[nodiscard]] static CommandHandler bind(State* state) noexcept
        {
            return {&commandDescriptor<C>(), [](void* owner, CommandInvocation& invocation) noexcept {
                CommandContext<C> context(invocation);
                return Function(*static_cast<State*>(owner), context.request(), context);
            }, state};
        }

    private:
        friend struct protocol_detail::CommandHandlerAudit;
        using Invoke = RenderResult<void> (*)(void*, CommandInvocation&) noexcept;
        CommandHandler(const CommandDescriptor* descriptor, Invoke invoke, void* user) noexcept
            : descriptor_(descriptor), invoke_(invoke), user_(user) {}
        const CommandDescriptor* descriptor_ = nullptr;
        Invoke invoke_ = nullptr;
        void* user_ = nullptr;
    };

    template <RenderCommand C, auto Function, class State>
        requires std::same_as<decltype(Function),
            RenderResult<void> (*)(State&, const typename C::Request&, CommandContext<C>&) noexcept>
    [[nodiscard]] CommandHandler bindCommand(State& state) noexcept
    {
        return CommandHandler::bind<C, Function>(&state);
    }

    [[nodiscard]] RenderResult<void> validateCommand(const CommandDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<void>
    compatibleCommands(const CommandDescriptor& left, const CommandDescriptor& right) noexcept;

    inline constexpr auto kProtocolContract = error::errorId("lux.render.protocol.contract");
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderProtocolErrorDescriptors() noexcept;
}

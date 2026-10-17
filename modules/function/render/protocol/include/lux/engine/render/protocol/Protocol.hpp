#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    struct CommandTag;
    struct HandlerTag;
    struct CommandNameTag;
    struct RequestTag;
    using CommandId = cxx::SlotKey<CommandTag>;
    using HandlerId = cxx::SlotKey<HandlerTag>;
    using CommandName = cxx::SchemaIdView<CommandNameTag>;
    using RequestId = cxx::SlotKey<RequestTag>;
    using SessionIdentity = std::array<std::uint64_t, 2>;

    struct Ack final {};

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
        CommandValueType request;
        CommandValueType response;
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
    inline constexpr CommandDescriptor kCommandDescriptor{
        CommandName(C::name),
        {cxx::typeToken<typename C::Request>(), sizeof(typename C::Request), alignof(typename C::Request)},
        {cxx::typeToken<typename C::Response>(), sizeof(typename C::Response), alignof(typename C::Response)}
    };

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

    inline constexpr auto kProtocolContract = error::errorId("lux.render.protocol.contract");

    struct CommandAsyncOps;
    struct CommandInvocation;
    namespace protocol_detail { struct ReplyAccess; struct ReplyAudit; }
    class CommandHandler;

    struct CommandCompletionFns final
    {
        RenderResult<void> (*finish)(void*, ECommandOutcome, RenderError, void*) noexcept;
        bool (*active)(const void*) noexcept;
        RenderResult<void> (*defer)(void*, void*, const CommandAsyncOps&) noexcept = nullptr;
        RenderResult<CommandInvocation*> (*promote)(void*, std::size_t) noexcept = nullptr;
        void (*abandon)(void*) noexcept = nullptr;
    };

    // A handler-call borrow. Asynchronous work first transfers its Reply to the stable pending slot.
    struct CommandInvocation final
    {
        const void* request_value;
        void* completion_owner;
        const CommandCompletionFns* completion;
    };

    template <RenderCommand C>
    class Reply final
    {
    public:
        Reply(const Reply&) = delete;
        Reply& operator=(const Reply&) = delete;
        Reply(Reply&& other) noexcept : invocation_(std::exchange(other.invocation_, nullptr)) {}
        Reply& operator=(Reply&&) = delete;
        ~Reply() noexcept
        {
            const auto* invocation = std::exchange(invocation_, nullptr);
            if (invocation && invocation->completion->abandon)
                invocation->completion->abandon(invocation->completion_owner);
        }

        [[nodiscard]] const typename C::Request& request() const noexcept
        {
            return *static_cast<const typename C::Request*>(invocation_->request_value);
        }

        [[nodiscard]] bool active() const noexcept
        {
            return invocation_ && invocation_->completion->active(invocation_->completion_owner);
        }

        // Capacity rejection preserves the response. After transfer, the original
        // reply owner retains it even if the peer closes before publication.
        [[nodiscard]] RenderResult<void> complete(typename C::Response& response) noexcept
        {
            return finish(ECommandOutcome::SUCCESS, {}, &response);
        }

        [[nodiscard]] RenderResult<void> complete(typename C::Response&& response) noexcept
        {
            return complete(response);
        }

        [[nodiscard]] RenderResult<void> fail(RenderError error) noexcept
        {
            return finish(ECommandOutcome::FAILED, error, nullptr);
        }

        [[nodiscard]] RenderResult<void> cancel() noexcept
        {
            return finish(ECommandOutcome::CANCELLED, {}, nullptr);
        }

        // Owner-thread promotion. On success the old SBO request borrow is invalid: reborrow
        // through the returned Reply before constructing any asynchronous operation.
        [[nodiscard]] RenderResult<Reply> defer(std::size_t reply_bytes = sizeof(typename C::Response)) & noexcept
        {
            if (!invocation_ || !invocation_->completion->promote)
                return cxx::unexpected(RenderError{kProtocolContract});
            auto promoted = invocation_->completion->promote(invocation_->completion_owner, reply_bytes);
            if (!promoted) return cxx::unexpected(promoted.error());
            invocation_ = nullptr;
            return Reply{**promoted};
        }

        // Defined by AsyncCommand.hpp for callers that actually create finite stdexec work.
        template <typename Sender>
        [[nodiscard]] RenderResult<void> completeWhen(Sender&& sender) noexcept;

    private:
        friend class CommandHandler;
        friend struct protocol_detail::ReplyAccess;
        friend struct protocol_detail::ReplyAudit;
        explicit Reply(CommandInvocation& invocation) noexcept : invocation_(&invocation) {}
        [[nodiscard]] RenderResult<void> finish(ECommandOutcome outcome, RenderError error, void* response) noexcept
        {
            auto* invocation = std::exchange(invocation_, nullptr);
            if (!invocation) return cxx::unexpected(RenderError{kProtocolContract});
            auto result = invocation->completion->finish(invocation->completion_owner, outcome, error, response);
            // Success may publish to another thread and destroy the operation containing this Reply.
            // A rejected completion publishes nothing; only that path may restore the capability.
            if (!result) invocation_ = invocation;
            return result;
        }

        CommandInvocation* invocation_;
    };

    namespace protocol_detail { struct CommandHandlerAudit; }

    // Non-owning active call. Installation owns the actual object separately.
    struct HandlerCall final
    {
        RenderResult<void> (*invoke)(void*, CommandInvocation&) noexcept = nullptr;
        void* object = nullptr;
    };

    class CommandHandler
    {
    public:
        CommandHandler() noexcept = default;
        [[nodiscard]] const CommandDescriptor* descriptor() const noexcept { return descriptor_; }
        [[nodiscard]] bool valid() const noexcept { return descriptor_ && call_.invoke; }
        [[nodiscard]] HandlerCall call() const noexcept { return call_; }
        [[nodiscard]] RenderResult<void> invoke(CommandInvocation& invocation) const noexcept
        {
            return call_.invoke(call_.object, invocation);
        }

        template <RenderCommand C, auto Function, class State>
            requires std::same_as<decltype(Function),
                RenderResult<void> (*)(State&, const typename C::Request&, Reply<C>&) noexcept>
        [[nodiscard]] static CommandHandler bind(State* state) noexcept
        {
            auto result = CommandHandler{&commandDescriptor<C>(), [](void* owner, CommandInvocation& invocation) noexcept {
                Reply<C> context(invocation);
                return Function(*static_cast<State*>(owner), context.request(), context);
            }, state};
            if (!state) result.call_.invoke = nullptr;
            return result;
        }

        template <RenderCommand C, auto Method, class Handler>
            requires std::same_as<decltype(Method),
                RenderResult<void> (Handler::*)(const typename C::Request&, Reply<C>&) noexcept>
        [[nodiscard]] static CommandHandler bind(Handler* handler) noexcept
        {
            auto result = CommandHandler{&commandDescriptor<C>(), [](void* object, CommandInvocation& invocation) noexcept {
                Reply<C> reply(invocation);
                return (static_cast<Handler*>(object)->*Method)(reply.request(), reply);
            }, handler};
            if (!handler) result.call_.invoke = nullptr;
            return result;
        }

        template <RenderCommand C, auto Method, class Handler>
            requires std::same_as<decltype(Method),
                void (Handler::*)(const typename C::Request&, Reply<C>) noexcept>
        [[nodiscard]] static CommandHandler bind(Handler* handler) noexcept
        {
            auto result = CommandHandler{&commandDescriptor<C>(), [](void* object, CommandInvocation& invocation) noexcept {
                Reply<C> reply(invocation);
                const auto& request = reply.request();
                (static_cast<Handler*>(object)->*Method)(request, std::move(reply));
                return RenderResult<void>{};
            }, handler};
            if (!handler) result.call_.invoke = nullptr;
            return result;
        }

    private:
        friend struct protocol_detail::CommandHandlerAudit;
        using Invoke = RenderResult<void> (*)(void*, CommandInvocation&) noexcept;
        CommandHandler(const CommandDescriptor* descriptor, Invoke invoke, void* user) noexcept
            : descriptor_(descriptor), call_{invoke, user} {}
        const CommandDescriptor* descriptor_ = nullptr;
        HandlerCall call_;
    };

    template <RenderCommand C, auto Function, class State>
        requires std::same_as<decltype(Function),
            RenderResult<void> (*)(State&, const typename C::Request&, Reply<C>&) noexcept>
    [[nodiscard]] CommandHandler bindCommand(State& state) noexcept
    {
        return CommandHandler::bind<C, Function>(&state);
    }

    template <RenderCommand C, auto Method, class Handler>
        requires (std::same_as<decltype(Method),
            void (Handler::*)(const typename C::Request&, Reply<C>) noexcept> ||
            std::same_as<decltype(Method),
            RenderResult<void> (Handler::*)(const typename C::Request&, Reply<C>&) noexcept>)
    [[nodiscard]] CommandHandler bindCommand(Handler& handler) noexcept
    {
        return CommandHandler::bind<C, Method>(&handler);
    }

    // Compile-time member list consumed by a typed installation; it has no runtime owner or registry.
    template <RenderCommand C, auto Method>
    struct CommandMethod final
    {
        using Command = C;
        template <class H>
        [[nodiscard]] static CommandHandler bind(H& handler) noexcept
        {
            return bindCommand<C, Method>(handler);
        }
    };

    [[nodiscard]] RenderResult<void> validateCommand(const CommandDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<void>
    compatibleCommands(const CommandDescriptor& left, const CommandDescriptor& right) noexcept;

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderProtocolErrorDescriptors() noexcept;
}

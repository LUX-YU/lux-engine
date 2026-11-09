#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <tuple>

namespace lux::render
{
    namespace extension_detail
    {
        template <class C, auto Method> struct CommandInvocation;

        template <class C, class H, void (H::*Method)(const typename C::Request&, typename C::Response&) noexcept>
        struct CommandInvocation<C, Method> final
        {
            using Handler = H;
            using Storage = void;
            using Result = void;
            static constexpr bool synchronous = true;

            static void
            invoke(H& handler, void*, const typename C::Request& request, typename C::Response& response) noexcept
            {
                (handler.*Method)(request, response);
            }
        };

        template <class C, class H, class R, R (H::*Method)(typename C::Request&&) noexcept>
        struct CommandInvocation<C, Method> final
        {
            using Handler = H;
            using Storage = void;
            using Result = R;
            static constexpr bool synchronous = false;

            static R invoke(H& handler, void*, typename C::Request&& request) noexcept
            {
                return (handler.*Method)(std::move(request));
            }
        };

        template <
            class C,
            class H,
            class S,
            void (H::*Method)(S&, const typename C::Request&, typename C::Response&) noexcept>
        struct CommandInvocation<C, Method> final
        {
            using Handler = H;
            using Storage = S;
            using Result = void;
            static constexpr bool synchronous = true;

            static void invoke(
                H& handler,
                void* storage,
                const typename C::Request& request,
                typename C::Response& response
            ) noexcept
            {
                (handler.*Method)(*static_cast<S*>(storage), request, response);
            }
        };

        template <class C, class H, class S, class R, R (H::*Method)(S&, typename C::Request&&) noexcept>
        struct CommandInvocation<C, Method> final
        {
            using Handler = H;
            using Storage = S;
            using Result = R;
            static constexpr bool synchronous = false;

            static R invoke(H& handler, void* storage, typename C::Request&& request) noexcept
            {
                return (handler.*Method)(*static_cast<S*>(storage), std::move(request));
            }
        };
    } // namespace extension_detail

    template <class C, class H, auto Method>
    concept SyncCommandHandlerFor =
        RenderCommand<C> && requires { typename extension_detail::CommandInvocation<C, Method>::Handler; } &&
        std::same_as<H, typename extension_detail::CommandInvocation<C, Method>::Handler> &&
        extension_detail::CommandInvocation<C, Method>::synchronous;

    // Compile-time association only. Runtime installation creates the actual call thunk.
    template <RenderCommand C, auto Method, std::uint32_t StorageDependency = UINT32_MAX> struct CommandMethod final
    {
    private:
        template <class Result, class H, class... Args> static H* handlerType(Result (H::*)(Args...) noexcept);

    public:
        using Command = C;
        using Handler = std::remove_pointer_t<decltype(handlerType(Method))>;
        static constexpr auto method = Method;
        static constexpr auto storage_dependency = StorageDependency;
    };
} // namespace lux::render

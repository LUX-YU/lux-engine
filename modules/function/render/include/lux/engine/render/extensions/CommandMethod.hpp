#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <tuple>

namespace lux::render
{
    namespace extension_detail
    {
        template <class C, auto Method, class Signature = decltype(Method)> struct CommandInvocation;

        template <class C, auto Method, class Request, class Response>
            requires std::same_as<Request, typename C::Request> && std::same_as<Response, typename C::Response>
        struct CommandInvocation<C, Method, void (*)(const Request&, Response&) noexcept> final
        {
            using Handler = void;
            using Storage = void;
            using Result = void;
            static constexpr bool synchronous = true;

            static void invoke(void*, const typename C::Request& request, typename C::Response& response) noexcept
            {
                Method(request, response);
            }
        };

        template <class C, auto Method, class R, class Request>
            requires std::same_as<Request, typename C::Request>
        struct CommandInvocation<C, Method, R (*)(Request&&) noexcept> final
        {
            using Handler = void;
            using Storage = void;
            using Result = R;
            static constexpr bool synchronous = false;

            static R invoke(void*, typename C::Request&& request) noexcept
            {
                return Method(std::move(request));
            }
        };

        template <class C, auto Method, class H, class Request, class Response>
            requires std::same_as<Request, typename C::Request> && std::same_as<Response, typename C::Response>
        struct CommandInvocation<C, Method, void (H::*)(const Request&, Response&) noexcept> final
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

        template <class C, auto Method, class H, class R, class Request>
            requires std::same_as<Request, typename C::Request>
        struct CommandInvocation<C, Method, R (H::*)(Request&&) noexcept> final
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

        template <class C, auto Method, class H, class S, class Request, class Response>
            requires std::same_as<Request, typename C::Request> && std::same_as<Response, typename C::Response>
        struct CommandInvocation<C, Method, void (H::*)(S&, const Request&, Response&) noexcept> final
        {
            using Handler = H;
            using Storage = S;
            using Result = void;
            static constexpr bool synchronous = true;

            static void
            invoke(
                H& handler,
                void* storage,
                const typename C::Request& request,
                typename C::Response& response
            ) noexcept
            {
                (handler.*Method)(*static_cast<S*>(storage), request, response);
            }
        };

        template <class C, auto Method, class H, class S, class R, class Request>
            requires std::same_as<Request, typename C::Request>
        struct CommandInvocation<C, Method, R (H::*)(S&, Request&&) noexcept> final
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

        template <class C, auto Method, class S, class Request, class Response>
            requires std::same_as<Request, typename C::Request> && std::same_as<Response, typename C::Response>
        struct CommandInvocation<C, Method, void (*)(S&, const Request&, Response&) noexcept> final
        {
            using Handler = void;
            using Storage = S;
            using Result = void;
            static constexpr bool synchronous = true;

            static void
            invoke(void* storage, const typename C::Request& request, typename C::Response& response) noexcept
            {
                Method(*static_cast<S*>(storage), request, response);
            }
        };

        template <class C, auto Method, class S, class R, class Request>
            requires std::same_as<Request, typename C::Request>
        struct CommandInvocation<C, Method, R (*)(S&, Request&&) noexcept> final
        {
            using Handler = void;
            using Storage = S;
            using Result = R;
            static constexpr bool synchronous = false;

            static R invoke(void* storage, typename C::Request&& request) noexcept
            {
                return Method(*static_cast<S*>(storage), std::move(request));
            }
        };
    } // namespace extension_detail

    template <class C, class H, auto Method>
    concept SyncCommandHandlerFor =
        RenderCommand<C> && requires { typename extension_detail::CommandInvocation<C, Method>::Handler; } &&
        std::same_as<H, typename extension_detail::CommandInvocation<C, Method>::Handler> &&
        extension_detail::CommandInvocation<C, Method>::synchronous;

    // Compile-time association only. Runtime installation creates the actual call thunk.
    template <RenderCommand C, auto Method> struct CommandMethod final
    {
        using Command = C;
        using Handler = typename extension_detail::CommandInvocation<C, Method>::Handler;
        static constexpr auto method = Method;
    };
} // namespace lux::render

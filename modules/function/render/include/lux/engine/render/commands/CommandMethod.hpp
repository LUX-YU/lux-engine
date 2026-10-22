#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <tuple>

namespace lux::render
{
    template <class C, class H, auto Method>
    concept SyncCommandHandlerFor =
        RenderCommand<C> &&
        std::same_as<decltype(Method), void (H::*)(const typename C::Request&, typename C::Response&) noexcept>;

    // Compile-time association only. Runtime installation creates the actual call thunk.
    template <RenderCommand C, auto Method> struct CommandMethod final
    {
    private:
        template <class Result, class H, class... Args> static H* handlerType(Result (H::*)(Args...) noexcept);

    public:
        using Command = C;
        using Handler = std::remove_pointer_t<decltype(handlerType(Method))>;
        static constexpr auto method = Method;
    };
} // namespace lux::render

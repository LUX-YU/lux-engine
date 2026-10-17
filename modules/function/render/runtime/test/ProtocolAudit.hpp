#pragma once

#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render::protocol_detail
{
    struct ReplyAudit final
    {
        template <RenderCommand C> static Reply<C> make(CommandInvocation& invocation) noexcept
        { return Reply<C>(invocation); }
    };

    struct CommandHandlerAudit final
    {
        static CommandHandler make(const CommandDescriptor* descriptor,
            RenderResult<void> (*invoke)(void*, CommandInvocation&) noexcept, void* user) noexcept
        {
            return {descriptor, invoke, user};
        }
        static CommandHandler owner(CommandHandler handler, void* user) noexcept
        {
            handler.call_.object = user;
            return handler;
        }
    };
}

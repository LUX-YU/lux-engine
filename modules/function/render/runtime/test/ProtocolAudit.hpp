#pragma once

#include <lux/engine/render/protocol/CommandHandler.hpp>

namespace lux::render::protocol_detail
{
    struct ReplyAudit final
    {
        template <RenderCommand C> static Reply<C> make(CommandInvocation& invocation) noexcept
        { return Reply<C>(invocation); }
    };

}

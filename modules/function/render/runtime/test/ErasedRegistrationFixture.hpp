#pragma once

#include <lux/engine/render/resource/Registration.hpp>

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

namespace lux::render::test_registration
{
    enum class EOwner { PROVIDED, DEVICE_SESSION };

    // Compatibility only inside historical fault fixtures, removed from production registration.
    inline CommandRegistration erased(const CommandDescriptor* descriptor,
        RenderResult<void> (*invoke)(void*, CommandInvocation&) noexcept,
        void* user = nullptr, EOwner owner = EOwner::PROVIDED) noexcept
    {
        // Native fixtures are migrated to nativeCommand before this branch may be used.
        if (owner != EOwner::PROVIDED) std::terminate();
        CommandRegistration result{protocol_detail::CommandHandlerAudit::make(descriptor, invoke, user)};
        return result;
    }
}

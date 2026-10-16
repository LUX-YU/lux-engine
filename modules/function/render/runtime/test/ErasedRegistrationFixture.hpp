#pragma once

#include <lux/engine/render/resource/Registration.hpp>

namespace lux::render::protocol_detail
{
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

namespace lux::render::registration_detail
{
    struct CommandRegistrationAudit final
    {
        // Tests requiring malformed descriptors/physical qualification deliberately bypass author API.
        // Each native thunk has its own static template instantiation, not a mutable global dispatch table.
        template <auto Invoke>
        static CommandRegistration native(const CommandDescriptor* descriptor, bool gpu) noexcept
        {
            CommandRegistration result{protocol_detail::CommandHandlerAudit::make(descriptor, Invoke, nullptr)};
            result.gpu_completion_ = gpu;
            return result;
        }
        static void gpu(CommandRegistration& value, bool enabled) noexcept { value.gpu_completion_ = enabled; }
    };
}

namespace lux::render::test_registration
{
    enum class EOwner { PROVIDED, DEVICE_SESSION };

    // Compatibility only inside historical fault fixtures, removed from production registration.
    inline CommandRegistration erased(const CommandDescriptor* descriptor,
        RenderResult<void> (*invoke)(void*, CommandInvocation&) noexcept,
        void* user = nullptr, EOwner owner = EOwner::PROVIDED, bool gpu = false) noexcept
    {
        // Native fixtures are migrated to nativeCommand before this branch may be used.
        if (owner != EOwner::PROVIDED) std::terminate();
        CommandRegistration result{protocol_detail::CommandHandlerAudit::make(descriptor, invoke, user)};
        registration_detail::CommandRegistrationAudit::gpu(result, gpu);
        return result;
    }
}

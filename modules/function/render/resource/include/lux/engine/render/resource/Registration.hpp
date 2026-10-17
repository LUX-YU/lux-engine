#pragma once

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render
{
    namespace registration_detail { struct NativeBinding; }

    class CommandRegistration final : public CommandHandler
    {
    public:
        CommandRegistration() noexcept = default;
        CommandRegistration(CommandHandler handler) noexcept : CommandHandler(handler) {}
        [[nodiscard]] bool valid() const noexcept { return descriptor() && (call().invoke || bind_native_); }
        void bindNative(void* owner) noexcept
        {
            if (bind_native_) static_cast<CommandHandler&>(*this) = bind_native_(owner);
        }

    private:
        friend struct registration_detail::NativeBinding;
        CommandHandler (*bind_native_)(void*) noexcept = nullptr;
    };

}

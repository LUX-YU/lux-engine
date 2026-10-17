#pragma once

#include <lux/engine/render/resource/Registration.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

namespace lux::render::registration_detail
{
    // Installed template implementation for external Native extension modules.
    struct NativeBinding final
    {
        template <RenderCommand C, auto Function>
        static CommandRegistration bind() noexcept
        {
            CommandRegistration result{CommandHandler::bind<C, Function>(static_cast<vulkan::DeviceSession*>(nullptr))};
            result.bind_native_ = [](void* owner) noexcept {
                return bindCommand<C, Function>(*static_cast<vulkan::DeviceSession*>(owner));
            };
            return result;
        }
    };
}

namespace lux::render::vulkan
{
    template <class Declaration> struct StorageProvider;

    template <RenderCommand C, auto Function>
        requires std::same_as<decltype(Function),
            RenderResult<void> (*)(DeviceSession&, const typename C::Request&, Reply<C>&) noexcept>
    [[nodiscard]] CommandRegistration nativeCommand() noexcept
    {
        return registration_detail::NativeBinding::bind<C, Function>();
    }
}

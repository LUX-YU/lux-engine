#pragma once

#include <vulkan/vulkan.h>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render::vulkan
{
    inline constexpr auto kNativeFailure = error::errorId("lux.render.vulkan.native_failure");
    inline constexpr auto kInvalidArgument = error::errorId("lux.render.vulkan.invalid_argument");
    inline constexpr auto kUnsupported = error::errorId("lux.render.vulkan.unsupported");
    inline constexpr auto kCapacity = error::errorId("lux.render.vulkan.capacity");
    inline constexpr auto kWrongOwner = error::errorId("lux.render.vulkan.wrong_owner");
    inline constexpr auto kBusy = error::errorId("lux.render.vulkan.busy");

    // Native result is encoded as its exact signed 32-bit bit pattern in args[0].
    [[nodiscard]] constexpr RenderError nativeError(VkResult result, std::uint64_t operation = 0) noexcept
    {
        return {kNativeFailure, {static_cast<std::uint32_t>(result), operation}};
    }

    [[nodiscard]] std::span<const error::ErrorDescriptor> vulkanErrorDescriptors() noexcept;
} // namespace lux::render::vulkan

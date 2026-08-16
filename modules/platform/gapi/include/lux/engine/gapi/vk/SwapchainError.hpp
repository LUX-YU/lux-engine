#pragma once
/**
 * @file SwapchainError.hpp
 * @brief Typed, exception-free failure vocabulary for WSI queries and
 *        swapchain construction.
 */

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>

namespace lux::gapi::vk
{
    enum class ESwapchainBuildStage : std::uint8_t
    {
        SURFACE_CAPABILITIES,
        SURFACE_FORMATS,
        SURFACE_PRESENT_MODES,
        CONFIGURE,
        CREATE,
        ENUMERATE_IMAGES,
        CREATE_IMAGE_VIEWS,
    };

    struct SwapchainBuildError final
    {
        ESwapchainBuildStage   stage{ESwapchainBuildStage::CONFIGURE};
        std::optional<VkResult> vk_result{};
    };

    [[nodiscard]] constexpr std::uint32_t encodeSwapchainBuildStage(
        ESwapchainBuildStage stage) noexcept
    {
        return static_cast<std::uint32_t>(stage);
    }
} // namespace lux::gapi::vk

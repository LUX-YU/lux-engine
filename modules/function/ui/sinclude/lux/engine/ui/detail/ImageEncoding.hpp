#pragma once

#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <cstdint>

namespace lux::ui::detail
{
    // ImTextureID is transport encoding only. Zero remains the private font token.
    [[nodiscard]] constexpr std::uint64_t encodeImage(render::RTextureHandle image) noexcept
    {
        return image.isValid() ? (std::uint64_t{image.gen} << 32U) | (std::uint64_t{image.index} + 1U) : 0;
    }

    [[nodiscard]] constexpr render::RTextureHandle decodeImage(std::uint64_t value) noexcept
    {
        if (value == 0U)
            return {};
        return render::RTextureHandle{static_cast<std::uint32_t>(value) - 1U, static_cast<std::uint32_t>(value >> 32U)};
    }
} // namespace lux::ui::detail

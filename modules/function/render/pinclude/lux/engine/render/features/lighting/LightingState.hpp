#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <lux/engine/render/frame/FrameError.hpp>

namespace lux::render::detail
{
    struct LightingState final
    {
        std::array<float, 4> environment{};

        [[nodiscard]] RenderResult<void> setEnvironment(const std::array<float, 3>& radiance) noexcept
        {
            const bool invalid = std::ranges::any_of(radiance, [](float channel) noexcept {
                return !std::isfinite(channel) || channel < 0;
            });
            if (invalid) return cxx::unexpected(RenderError{kFrameContract});
            std::ranges::copy(radiance, environment.begin());
            return {};
        }
    };
}

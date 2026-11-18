#pragma once

#include <cmath>

namespace lux::render
{
    struct EffectParameters final
    {
        float exposure = 1;

        [[nodiscard]] bool valid() const noexcept
        {
            return std::isfinite(exposure);
        }

        bool operator==(const EffectParameters&) const noexcept = default;
    };
} // namespace lux::render

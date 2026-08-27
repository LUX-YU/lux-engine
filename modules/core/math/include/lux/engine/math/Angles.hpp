#pragma once
#include <concepts>
#include <numbers>

namespace lux::math
{

    template <std::floating_point T> [[nodiscard]] constexpr T deg_to_rad(T deg) noexcept
    {
        return deg * std::numbers::pi_v<T> / T(180);
    }

    template <std::floating_point T> [[nodiscard]] constexpr T rad_to_deg(T rad) noexcept
    {
        return rad * T(180) / std::numbers::pi_v<T>;
    }

} // namespace lux::math

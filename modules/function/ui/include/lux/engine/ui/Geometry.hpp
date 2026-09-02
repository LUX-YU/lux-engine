#pragma once

#include <compare>

namespace lux::ui
{
    struct Vec2 final
    {
        float x{};
        float y{};

        [[nodiscard]] constexpr bool operator==(const Vec2&) const noexcept = default;
    };

    struct Point final
    {
        float x{};
        float y{};

        [[nodiscard]] constexpr bool operator==(const Point&) const noexcept = default;
    };

    struct Size final
    {
        float width{};
        float height{};

        [[nodiscard]] constexpr bool operator==(const Size&) const noexcept = default;
    };

    struct Color final
    {
        float red{};
        float green{};
        float blue{};
        float alpha{1.0F};

        [[nodiscard]] constexpr bool operator==(const Color&) const noexcept = default;
    };
} // namespace lux::ui

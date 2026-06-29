#pragma once
/**
 * @file FeatureHandle.hpp
 * @brief Strong-typed handles for render features and views.
 */

#include <cstdint>
#include <compare>
#include <limits>

namespace lux::render
{
    /// Strong-typed feature identifier.
    struct FeatureHandle
    {
        uint32_t id{std::numeric_limits<uint32_t>::max()};

        [[nodiscard]] bool valid() const noexcept
        {
            return id != std::numeric_limits<uint32_t>::max();
        }

        explicit operator bool() const noexcept { return valid(); }
        auto operator<=>(const FeatureHandle &) const = default;
    };

    /// Strong-typed view identifier.
    struct ViewHandle
    {
        uint32_t id{std::numeric_limits<uint32_t>::max()};

        [[nodiscard]] bool valid() const noexcept
        {
            return id != std::numeric_limits<uint32_t>::max();
        }

        explicit operator bool() const noexcept { return valid(); }
        auto operator<=>(const ViewHandle &) const = default;
    };
} // namespace lux::render

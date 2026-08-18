#pragma once

#include <lux/cxx/core/StableNameId.hpp>

#include <string_view>

namespace lux::render
{
    /// Stable identity for one render effect. This ID belongs to the Render
    /// domain and is intentionally not interchangeable with Extension,
    /// Editor Panel, or Scene Feature identities.
    struct RenderEffectIdTag final {};

    using RenderEffectIdView = lux::cxx::StableNameIdView<RenderEffectIdTag>;
    using RenderEffectId = lux::cxx::StableNameId<RenderEffectIdTag>;

    [[nodiscard]] constexpr RenderEffectIdView renderEffectId(
        std::string_view name) noexcept
    {
        return RenderEffectIdView{name};
    }

    [[nodiscard]] inline bool isValidRenderEffectIdName(
        std::string_view name) noexcept
    {
        if (name.empty() || name.front() == '.' || name.back() == '.')
            return false;

        bool has_dot = false;
        bool previous_dot = false;
        for (const char value : name)
        {
            const bool dot = value == '.';
            if (dot)
            {
                if (previous_dot)
                    return false;
                has_dot = true;
            }
            else if (!((value >= 'a' && value <= 'z') ||
                       (value >= '0' && value <= '9') ||
                       value == '_' || value == '-'))
            {
                return false;
            }
            previous_dot = dot;
        }
        return has_dot;
    }

    [[nodiscard]] inline bool renderEffectIdCollision(
        RenderEffectIdView lhs,
        RenderEffectIdView rhs) noexcept
    {
        return lhs.hash() == rhs.hash() && lhs.name() != rhs.name();
    }
} // namespace lux::render

#pragma once

#include <lux/cxx/core/StableNameId.hpp>

#include <string_view>

namespace lux::editor
{
    /// Stable identity for one editor panel kind. Panel identity belongs to the
    /// Editor domain; it is deliberately distinct from renderer feature and
    /// Extension identities.
    struct PanelIdTag final {};

    using PanelIdView = lux::cxx::StableNameIdView<PanelIdTag>;
    using PanelId = lux::cxx::StableNameId<PanelIdTag>;

    [[nodiscard]] constexpr PanelIdView panelId(
        std::string_view name) noexcept
    {
        return PanelIdView{name};
    }

    [[nodiscard]] inline bool isValidPanelIdName(
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

    [[nodiscard]] inline bool panelIdCollision(
        PanelIdView lhs,
        PanelIdView rhs) noexcept
    {
        return lhs.hash() == rhs.hash() && lhs.name() != rhs.name();
    }
} // namespace lux::editor

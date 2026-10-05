#pragma once
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/editor/sessions/SessionId.hpp>
#include <vector>
#include <optional>

namespace lux::ui
{
    struct PaneTypeIdTag;
}

namespace lux::editor::views
{
    using ViewTypeIdView = lux::cxx::StableNameIdView<lux::ui::PaneTypeIdTag>;
    using ViewTypeId = lux::cxx::StableNameId<lux::ui::PaneTypeIdTag>;
    struct ViewRestoreKeyTag final
    {
    };
    using ViewRestoreKey = lux::cxx::StableNameId<ViewRestoreKeyTag>;
    // Content association is independent of window kind. Empty tool windows and comparison views
    // use the same value; the primary target must be one of the explicitly associated sessions.
    struct ViewContent final
    {
        std::vector<sessions::SessionId> sessions;
        std::optional<sessions::SessionId> primary;
        [[nodiscard]] bool valid() const noexcept
        {
            if (sessions.size() > 64)
                return false;
            bool has_primary = !primary;
            for (std::size_t i{}; i < sessions.size(); ++i)
            {
                if (!sessions[i].valid())
                    return false;
                has_primary = has_primary || primary == sessions[i];
                for (std::size_t j{}; j < i; ++j)
                    if (sessions[j] == sessions[i])
                        return false;
            }
            return has_primary;
        }
        friend bool operator==(const ViewContent&, const ViewContent&) = default;
    };
} // namespace lux::editor::views

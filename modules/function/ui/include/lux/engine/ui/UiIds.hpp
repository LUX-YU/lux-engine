#pragma once

// Stable semantic identities shared by the final UI Foundation primitives.

#include <lux/cxx/core/StableNameId.hpp>

namespace lux::ui
{
    struct PaneIdTag final
    {
    };
    struct PaneTypeIdTag final
    {
    };
    struct UiContextIdTag final
    {
    };
    struct UiCommandIdTag final
    {
    };
    struct PayloadTypeIdTag final
    {
    };

    using PaneId = lux::cxx::StableNameId<PaneIdTag>;
    using PaneIdView = lux::cxx::StableNameIdView<PaneIdTag>;
    using PaneTypeId = lux::cxx::StableNameId<PaneTypeIdTag>;
    using PaneTypeIdView = lux::cxx::StableNameIdView<PaneTypeIdTag>;
    using UiContextId = lux::cxx::StableNameId<UiContextIdTag>;
    using UiContextIdView = lux::cxx::StableNameIdView<UiContextIdTag>;
    using UiCommandId = lux::cxx::StableNameId<UiCommandIdTag>;
    using UiCommandIdView = lux::cxx::StableNameIdView<UiCommandIdTag>;
    using PayloadTypeId = lux::cxx::StableNameId<PayloadTypeIdTag>;
    using PayloadTypeIdView = lux::cxx::StableNameIdView<PayloadTypeIdTag>;

    inline constexpr UiContextIdView kGlobalContext{"lux.ui.global"};
} // namespace lux::ui

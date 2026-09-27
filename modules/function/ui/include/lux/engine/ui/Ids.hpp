#pragma once

// Stable semantic identities shared by the final UI Foundation primitives.

#include <lux/cxx/core/StableNameId.hpp>

namespace lux::ui
{
    struct PaneIdTag final
    {};
    struct PaneTypeIdTag final
    {};
    struct CommandIdTag final
    {};
    struct PayloadTypeIdTag final
    {};
    struct ElementIdTag final
    {};

    using PaneId = lux::cxx::StableNameId<PaneIdTag>;
    using PaneIdView = lux::cxx::StableNameIdView<PaneIdTag>;
    using PaneTypeId = lux::cxx::StableNameId<PaneTypeIdTag>;
    using PaneTypeIdView = lux::cxx::StableNameIdView<PaneTypeIdTag>;
    using CommandId = lux::cxx::StableNameId<CommandIdTag>;
    using CommandIdView = lux::cxx::StableNameIdView<CommandIdTag>;
    using PayloadTypeId = lux::cxx::StableNameId<PayloadTypeIdTag>;
    using PayloadTypeIdView = lux::cxx::StableNameIdView<PayloadTypeIdTag>;
    using ElementId = lux::cxx::StableNameId<ElementIdTag>;
    using ElementIdView = lux::cxx::StableNameIdView<ElementIdTag>;

} // namespace lux::ui

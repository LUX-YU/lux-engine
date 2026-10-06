#pragma once

// PaneId is local to one Root registration; other IDs name content and commands.

#include <lux/cxx/core/StableNameId.hpp>
#include <lux/cxx/container/SlotMap.hpp>

namespace lux::ui
{
    struct PaneIdTag final
    {};
    struct CommandIdTag final
    {};
    struct PayloadTypeIdTag final
    {};
    struct ElementIdTag final
    {};

    using PaneId = lux::cxx::SlotKey<PaneIdTag>;
    using CommandId = lux::cxx::StableNameId<CommandIdTag>;
    using CommandIdView = lux::cxx::StableNameIdView<CommandIdTag>;
    using PayloadTypeId = lux::cxx::StableNameId<PayloadTypeIdTag>;
    using PayloadTypeIdView = lux::cxx::StableNameIdView<PayloadTypeIdTag>;
    using ElementId = lux::cxx::StableNameId<ElementIdTag>;
    using ElementIdView = lux::cxx::StableNameIdView<ElementIdTag>;

} // namespace lux::ui

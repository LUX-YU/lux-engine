#pragma once

// PaneId is local to one Root registration; other IDs name content and commands.

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/core/StableNameId.hpp>

namespace lux::ui
{
    struct PaneIdTag final
    {
    };
    struct CommandIdTag final
    {
    };
    struct PayloadTypeIdTag final
    {
    };

    using PaneId = lux::cxx::SlotKey<PaneIdTag>;
    using CommandId = lux::cxx::StableNameId<CommandIdTag>;
    using CommandIdView = lux::cxx::StableNameIdView<CommandIdTag>;
    using PayloadTypeId = lux::cxx::StableNameId<PayloadTypeIdTag>;
    using PayloadTypeIdView = lux::cxx::StableNameIdView<PayloadTypeIdTag>;

} // namespace lux::ui

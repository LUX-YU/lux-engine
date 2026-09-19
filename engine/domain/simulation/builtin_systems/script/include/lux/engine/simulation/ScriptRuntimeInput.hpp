#pragma once

#include <lux/engine/simulation/ScriptBinding.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/script_system/visibility.h>

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace lux::simulation::script
{
    struct ScriptRuntimeMount final
    {
        ScriptMountId id;
        lux::asset::AssetId asset;
        ScriptInstanceScope scope;
        std::vector<ScriptBindingDescription> bindings;
        // Original finite composition position, including disabled/pending positions in a loader description.
        // Omission is allowed for a complete initial batch or a known ID; first late admission must name it.
        std::uint32_t configuration_index{std::numeric_limits<std::uint32_t>::max()};
    };

    struct ScriptEndpointCapacity final
    {
        ScriptBindingTarget target;
        std::size_t handler_capacity{};
    };

    struct ScriptRuntimeCapacityPlan final
    {
        std::size_t mount_capacity{};
        std::size_t enabled_mount_capacity{};
        std::size_t binding_capacity{};
        std::size_t method_capacity{};
        std::vector<ScriptEndpointCapacity> endpoint_capacities;
    };
}

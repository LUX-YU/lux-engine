#pragma once

#include <lux/engine/simulation/ScriptMountDescription.hpp>

#include <vector>

namespace lux::simulation
{
    // Serializable ECS/World fact. Runtime calls, backend state, leases and
    // dense dispatch caches deliberately live in ScriptBindingSession instead.
    struct ScriptMountFacts final
    {
        std::vector<ScriptMountDescription> mounts;

        friend bool operator==(const ScriptMountFacts&, const ScriptMountFacts&)
            noexcept = default;
    };
}

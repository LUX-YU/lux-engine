#pragma once

#include <lux/engine/simulation/ScriptMountDescription.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace lux::simulation
{
    inline constexpr std::string_view ScriptComponentCanonicalName{
        "lux.simulation.ScriptComponent"};
    inline constexpr std::uint32_t ScriptComponentSchemaVersion{2U};

    struct ScriptComponent final
    {
        std::vector<ScriptMountDescription> mounts;

        [[nodiscard]] bool valid() const noexcept
        {
            return validScriptMountList(mounts);
        }

        friend bool operator==(
            const ScriptComponent&,
            const ScriptComponent&
        ) noexcept = default;
    };
}

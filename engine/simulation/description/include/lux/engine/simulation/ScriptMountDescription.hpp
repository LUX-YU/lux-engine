#pragma once

#include <lux/engine/description/Script.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>

#include <cstdint>
#include <vector>

namespace lux::simulation
{
    enum class EScriptBindingSetMode : std::uint8_t
    {
        ASSET_DEFAULTS,
        EXPLICIT,
    };

    struct ScriptMountDescription final
    {
        lux::asset::AssetId script;
        EScriptBindingSetMode binding_mode{
            EScriptBindingSetMode::ASSET_DEFAULTS};
        std::vector<lux::rdesc::ScriptBindingDescription> bindings;

        friend bool operator==(
            const ScriptMountDescription&,
            const ScriptMountDescription&
        ) noexcept = default;
    };

    [[nodiscard]] inline bool validScriptMountDescription(
        const ScriptMountDescription& mount
    ) noexcept
    {
        if (mount.script.isNull() ||
            (mount.binding_mode == EScriptBindingSetMode::ASSET_DEFAULTS &&
             !mount.bindings.empty()))
        {
            return false;
        }
        for (std::size_t index{}; index < mount.bindings.size(); ++index)
        {
            const auto& binding = mount.bindings[index];
            if (binding.function == lux::script::InvalidScriptSymbolId ||
                binding.system_type.empty() || binding.member.empty())
            {
                return false;
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (mount.bindings[previous] == binding)
                    return false;
            }
        }
        return true;
    }
}

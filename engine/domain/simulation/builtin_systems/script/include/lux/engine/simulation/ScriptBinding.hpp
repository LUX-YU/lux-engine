#pragma once

#include <lux/engine/function/script/ScriptSymbol.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <compare>
#include <cstdint>
#include <variant>

namespace lux::simulation::script
{
    struct ScriptMountId final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        friend constexpr auto operator<=>(ScriptMountId, ScriptMountId) noexcept = default;
    };

    struct HookScriptTarget final
    {
        lux::system::SystemInstanceId system;
        HookPointId      hook;

        friend constexpr bool operator==(HookScriptTarget, HookScriptTarget) noexcept = default;
    };

    struct EventScriptTarget final
    {
        lux::system::SystemInstanceId system;
        EventPointId     event;

        friend constexpr bool operator==(EventScriptTarget, EventScriptTarget) noexcept = default;
    };

    using ScriptBindingTarget = std::variant<
        HookScriptTarget,
        EventScriptTarget
    >;

    struct ScriptBindingDescription final
    {
        lux::script::ScriptSymbolId symbol{lux::script::InvalidScriptSymbolId};
        ScriptBindingTarget         target;

        friend bool operator==(const ScriptBindingDescription&, const ScriptBindingDescription&) noexcept = default;
    };

}

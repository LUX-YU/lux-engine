#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace lux::flowforge
{
    struct ScriptAbilityNodeDescription final
    {
        lux::script::ScriptApiContractIdView contract;
        lux::script::ScriptApiMethodIdView method;
        std::uint64_t schema_hash{};
        lux::script::EScriptApiMethodKind kind{lux::script::EScriptApiMethodKind::QUERY};
        std::span<const lux::script::ScriptAbilityParameterDescription> parameters;
        std::span<const lux::script::ScriptAbilityValueDescription> results;
    };

    template <class Ability>
    struct ScriptAbilityCatalog final
    {
        using Traits = lux::script::ScriptAbilityTraits<Ability>;

        inline static constexpr auto Nodes = []() consteval {
            std::array<ScriptAbilityNodeDescription, Traits::Methods.size()> result{};
            for (std::size_t index{}; index < result.size(); ++index)
            {
                const auto& method = Traits::Methods[index];
                result[index] = {
                    Traits::Description.id,
                    method.id,
                    Traits::Description.schema_hash,
                    method.kind,
                    method.parameters,
                    method.results
                };
            }
            return result;
        }();
    };

    template <class Ability>
    [[nodiscard]] constexpr std::span<const ScriptAbilityNodeDescription> scriptAbilityNodes() noexcept
    {
        return ScriptAbilityCatalog<Ability>::Nodes;
    }
} // namespace lux::flowforge

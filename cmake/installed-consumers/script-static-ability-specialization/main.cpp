#include "StaticAbility.hpp"
#include "StaticAbility.ability.generated.hpp"

#include <cassert>
#include <cstdint>

namespace
{
    struct Provider final
    {
        std::int32_t value{4};

        std::int32_t read(std::int32_t input) noexcept
        {
            return value + input;
        }
    };
}

int main()
{
    using Ability = installed_consumer::StaticAbility;
    Provider provider;
    const auto binding = lux::script::bindScriptAbility<Ability>(provider);
    auto specialized = lux::script::ScriptAbilityStatic<Ability, Provider>::create(provider, binding);
    assert(specialized && specialized->read(3) == 7);

    auto mismatched = lux::script::ScriptAbilityTraits<Ability>::Description;
    mismatched.schema_hash ^= 1U;
    const lux::script::ScriptAbilityBinding wrong{
        &mismatched,
        binding.context,
        binding.dispatch,
        binding.erased_methods
    };
    assert((!lux::script::ScriptAbilityStatic<Ability, Provider>::create(provider, wrong)));
    return 0;
}

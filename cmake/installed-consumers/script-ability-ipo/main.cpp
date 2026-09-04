#include "TinyAbility.hpp"
#include "TinyAbility.ability.generated.hpp"
#include "TinyAbility.ability.native.generated.hpp"

#include <cassert>
#include <cstdint>

namespace
{
    struct Provider final
    {
        std::int32_t bias{4};

        [[nodiscard]] std::int32_t read(std::int32_t input) noexcept
        {
            return bias + input;
        }
    };
}

int main()
{
    using Ability = installed_consumer::TinyAbility;
    Provider provider;
    const auto binding = lux::script::bindScriptAbility<Ability>(provider);
    const auto dynamic = lux::script::ScriptAbilityCpp<Ability>::create(binding);
    const auto specialized = lux::script::ScriptAbilityStatic<Ability, Provider>::create(provider, binding);
    const auto native = lux::script::native::makeScriptAbilityNativeContribution<Ability>();
    assert(dynamic && specialized && native.valid() && native.methods.size() == 1U);
    using NativeRead = std::int32_t (*)(void*, const void*, std::int32_t) noexcept;
    const auto native_read = reinterpret_cast<NativeRead>(native.methods.front().entry);
    assert(provider.read(3) == 7);
    assert(dynamic->read(3) == 7);
    assert(specialized->read(3) == 7);
    assert(native_read(binding.context, binding.dispatch, 3) == 7);
    return 0;
}

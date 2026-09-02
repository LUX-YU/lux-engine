#include "InventoryAbility.hpp"
#include "InventoryAbility.ability.generated.hpp"

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <cassert>
#include <cstdint>

namespace
{
    struct InventoryProvider final
    {
        std::uint64_t last_item{};
        std::int32_t value{};

        std::int32_t count(std::uint64_t item) noexcept
        {
            last_item = item;
            return value;
        }

        void setCount(std::uint64_t item, std::int32_t count) noexcept
        {
            last_item = item;
            value = count;
        }
    };

    struct InvalidProvider final
    {
        void count(std::uint64_t) noexcept
        {
        }
    };
}

int main()
{
    using Ability = installed_consumer::InventoryAbility;
    using Traits = lux::script::ScriptAbilityTraits<Ability>;
    static_assert(Traits::ProviderConforms<InventoryProvider>);
    static_assert(!Traits::ProviderConforms<InvalidProvider>);
    static_assert(Traits::Description.schema_hash != 0U);

    InventoryProvider provider;
    const auto binding = lux::script::bindScriptAbility<Ability>(provider);
    auto api = lux::script::ScriptAbilityCpp<Ability>::create(binding);
    assert(api);
    api->setCount(17U, 4);
    assert(api->count(17U) == 4);
    assert(provider.last_item == 17U);
    return 0;
}

#include "TestAbility.hpp"
#include "TestAbility.ability.generated.hpp"

#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/function/script/lua/ScriptAbilityLua.hpp>

#include <cassert>
#include <cstdint>
#include <string_view>

namespace
{
    struct TestProvider final
    {
        std::int32_t value{7};
        std::size_t calls{};

        std::int32_t readValue(std::int32_t input) noexcept
        {
            ++calls;
            return value + input;
        }

        void setValue(std::int32_t new_value) noexcept
        {
            ++calls;
            value = new_value;
        }

        std::uint64_t identity(std::uint64_t input) noexcept
        {
            return input;
        }

        const std::int32_t& borrowedValue() noexcept
        {
            return value;
        }

        lux::script::ScriptAbilityStartResult beginOperation(
            std::uint64_t,
            lux::script::ScriptAbilityCompletion<std::uint64_t>
        ) noexcept
        {
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{74});
        }
    };
}

int main()
{
    using Ability = lux::simulation::test::TestAbility;
    using Traits = lux::script::ScriptAbilityTraits<Ability>;

    const auto nodes = lux::flowforge::scriptAbilityNodes<Ability>();
    assert(nodes.size() == Traits::Description.methods.size());
    for (std::size_t index{}; index < nodes.size(); ++index)
    {
        assert(nodes[index].contract == Traits::Description.id);
        assert(nodes[index].method == Traits::Description.methods[index].id);
        assert(nodes[index].schema_hash == Traits::Description.schema_hash);
        assert(nodes[index].kind == Traits::Description.methods[index].kind);
    }

    return 0;
}
